// SPDX-License-Identifier: Apache-2.0
/*
Copyright (C) 2024 The Falco Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#include <sys/stat.h>

#include <libsinsp/container_engine/containerd.h>
#include <libsinsp/cri.h>
#include <libsinsp/grpc_channel_registry.h>
#include <libsinsp/runc.h>
#include <libsinsp/sinsp.h>

using namespace libsinsp::container_engine;
using namespace libsinsp::runc;

constexpr const cgroup_layout CONTAINERD_CGROUP_LAYOUT[] = {{"/default/", ""}, {nullptr, nullptr}};

constexpr const std::string_view CONTAINERD_SOCKETS[] = {
        "/run/host-containerd/containerd.sock",      // bottlerocket host containers socket
        "/run/containerd/runtime2/containerd.sock",  // tmp
};

bool containerd_interface::is_ok() {
	return m_stub != nullptr;
}
containerd_interface::containerd_interface(const std::string &socket_path) {
	grpc::ChannelArguments args;
	args.SetInt(GRPC_ARG_ENABLE_HTTP_PROXY, 0);
	std::shared_ptr<grpc::Channel> channel =
	        libsinsp::grpc_channel_registry::get_channel("unix://" + socket_path, &args);

	m_stub = ContainerdService::Containers::NewStub(channel);

	ContainerdService::ListContainersRequest req;
	ContainerdService::ListContainersResponse resp;

	grpc::ClientContext context;
	auto deadline = std::chrono::system_clock::now() +
	                std::chrono::milliseconds(libsinsp::cri::cri_settings::get_cri_timeout());
	context.set_deadline(deadline);

	// The `default` namesapce is the default one of containerd
	// and the one used by host-containers in bottlerocket.
	// This is mandatory to query the containers.
	context.AddMetadata("containerd-namespace", "default");
	grpc::Status status = m_stub->List(&context, req, &resp);

	if(!status.ok()) {
		libsinsp_logger()->format(sinsp_logger::SEV_NOTICE,
		                          "containerd (%s): containerd runtime returned an error after "
		                          "trying to list containerd: %s",
		                          socket_path.c_str(),
		                          status.error_message().c_str());
		m_stub.reset(nullptr);
		return;
	}
}

grpc::Status containerd_interface::list_container_resp(
        const std::string &container_id,
        ContainerdService::ListContainersResponse &resp) {
	ContainerdService::ListContainersRequest req;

	// To match the container using a truncated containerd id
	// we need to use a match filter (~=).
	req.add_filters("id~=" + container_id);
	grpc::ClientContext context;
	context.AddMetadata("containerd-namespace", "default");
	auto deadline = std::chrono::system_clock::now() +
	                std::chrono::milliseconds(libsinsp::cri::cri_settings::get_cri_timeout());
	context.set_deadline(deadline);
	return m_stub->List(&context, req, &resp);
}

libsinsp::container_engine::containerd::containerd(container_cache_interface &cache):
        container_engine_base(cache) {
	for(const auto &p : CONTAINERD_SOCKETS) {
		if(p.empty()) {
			continue;
		}

		auto socket_path = scap_get_host_root() + std::string(p);
		struct stat s = {};
		if(stat(socket_path.c_str(), &s) != 0 || (s.st_mode & S_IFMT) != S_IFSOCK) {
			continue;
		}

		m_interface = std::make_unique<containerd_interface>(socket_path);
		if(!m_interface->is_ok()) {
			m_interface.reset(nullptr);
			continue;
		}
	}
}

bool libsinsp::container_engine::containerd::parse_containerd(sinsp_container_info &container,
                                                              const std::string &container_id) {
	// given the truncated container id, the full container id needs to be retrivied from
	// containerd.
	ContainerdService::ListContainersResponse resp;
	grpc::Status status = m_interface->list_container_resp(container_id, resp);

	if(!status.ok()) {
		libsinsp_logger()->format(
		        sinsp_logger::SEV_DEBUG,
		        "containerd (%s): ListContainerResponse status error message: (%s)",
		        container.m_id.c_str(),
		        status.error_message().c_str());
		return false;
	}

	auto containers = resp.containers();

	if(containers.size() == 0) {
		libsinsp_logger()->format(sinsp_logger::SEV_DEBUG,
		                          "containerd (%s): ListContainerResponse status error message: "
		                          "(container id has no match)",
		                          container.m_id.c_str());
		return false;
	} else if(containers.size() > 1) {
		libsinsp_logger()->format(sinsp_logger::SEV_DEBUG,
		                          "containerd (%s): ListContainerResponse status error message: "
		                          "(container id has more than one match)",
		                          container.m_id.c_str());
		return false;
	}

	// Usually the image has this form: `docker.io/library/ubuntu:22.04`
	auto raw_image_splits = sinsp_split(containers[0].image(), ':');

	container.m_id = container_id;
	container.m_full_id = containers[0].id();
	// We assume that the last `/`-separated field is the image
	container.m_image = raw_image_splits[0].substr(raw_image_splits[0].rfind("/") + 1);
	// and the first part is the repo
	container.m_imagerepo = raw_image_splits[0].substr(0, raw_image_splits[0].rfind("/"));
	container.m_imagetag = raw_image_splits[1];
	container.m_imagedigest = "";
	container.m_type = CT_CONTAINERD;

	// Retrieve the labels.
	for(const auto &pair : containers[0].labels()) {
		if(pair.second.length() <= sinsp_container_info::m_container_label_max_length) {
			container.m_labels[pair.first] = pair.second;
		}
	}

	// The spec field keeps the information about the mounts.
	Json::Value spec;
	Json::Reader reader;
	// The spec field of the response is just a raw json.
	reader.parse(containers[0].spec().value(), spec);

	// Retrieve the mounts.
	for(const auto &m : spec["mounts"]) {
		bool readonly = false;
		std::string mode;
		for(const auto &jopt : m["options"]) {
			std::string opt = jopt.asString();
			if(opt == "ro") {
				readonly = true;
			} else if(opt.rfind("mode=") == 0) {
				mode = opt.substr(5);
			}
		}
		container.m_mounts.emplace_back(m["source"].asString(),
		                                m["destination"].asString(),
		                                mode,
		                                !readonly,
		                                spec["linux"]["rootfsPropagation"].asString());
	}

	// Retrieve the env.
	for(const auto &env : spec["process"]["env"]) {
		container.m_env.emplace_back(env.asString());
	}

	return true;
}

bool libsinsp::container_engine::containerd::resolve(sinsp_threadinfo *tinfo,
                                                     bool query_os_for_missing_info) {
	auto container = sinsp_container_info();
	std::string container_id, cgroup;

	if(!matches_runc_cgroups(tinfo, CONTAINERD_CGROUP_LAYOUT, container_id, cgroup)) {
		return false;
	}

	if(!parse_containerd(container, container_id)) {
		return false;
	}

	tinfo->m_container_id = container_id;

	libsinsp::cgroup_limits::cgroup_limits_key key(container.m_id,
	                                               tinfo->get_cgroup("cpu"),
	                                               tinfo->get_cgroup("memory"),
	                                               tinfo->get_cgroup("cpuset"));

	libsinsp::cgroup_limits::cgroup_limits_value limits;
	libsinsp::cgroup_limits::get_cgroup_resource_limits(key, limits);

	container.m_memory_limit = limits.m_memory_limit;
	container.m_cpu_shares = limits.m_cpu_shares;
	container.m_cpu_quota = limits.m_cpu_quota;
	container.m_cpu_period = limits.m_cpu_period;
	container.m_cpuset_cpu_count = limits.m_cpuset_cpu_count;

	if(container_cache().should_lookup(container.m_id, CT_CONTAINERD)) {
		container.m_name = container.m_id;
		container.set_lookup_status(sinsp_container_lookup::state::SUCCESSFUL);
		container_cache().add_container(std::make_shared<sinsp_container_info>(container), tinfo);
		container_cache().notify_new_container(container, tinfo);
	}
	return true;
}
