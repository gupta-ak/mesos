// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __CHECKER_PROCESS_HPP__
#define __CHECKER_PROCESS_HPP__

#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/protobuf.hpp>

#include <stout/duration.hpp>
#include <stout/option.hpp>
#include <stout/stopwatch.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace checks {

// ContainerRuntime describes the type of containerizer that called the health
// check.
enum class ContainerRuntime
{
    UCR,
    DOCKER,
    MESOS
};

// DockerRuntimeInfo contains the necessary docker runtime information
// to run the health check.
struct DockerRuntimeInfo
{
    std::string dockerPath;
    std::string socketName;
    std::string containerName;
};

// ContainerRuntimeInfo contains the containerizer-specific runtime
// information to run the health check.
struct ContainerRuntimeInfo
{
    ContainerRuntime type;

    // Right now only the docker containizer on Windows needs runtime
    // information, but future Windows containiers like UCR might also
    // need runtime specific information.
    DockerRuntimeInfo dockerInfo;
};

#ifdef __WINDOWS__
constexpr char DOCKER_HEALTH_CHECK_IMAGE[] = "microsoft/powershell:nanoserver";
#endif // __WINDOWS__

class CheckerProcess : public ProtobufProcess<CheckerProcess>
{
public:
  // TODO(gkleiman): Instead of passing an optional scheme as a parameter,
  // consider introducing a global `TLSInfo` protobuf and using it in HTTP
  // checks. See MESOS-7356.
  CheckerProcess(
      const CheckInfo& _check,
      const std::string& _launcherDir,
      const lambda::function<void(const Try<CheckStatusInfo>&)>& _callback,
      const TaskID& _taskId,
      const Option<pid_t>& _taskPid,
      const std::vector<std::string>& _namespaces,
      const Option<ContainerID>& _taskContainerId,
      const Option<process::http::URL>& _agentURL,
      const Option<std::string>& _authorizationHeader,
      const Option<std::string>& _scheme,
      const std::string& _name,
      const ContainerRuntimeInfo& _runtime,
      bool _commandCheckViaAgent,
      bool _ipv6 = false);

  void pause();
  void resume();

  virtual ~CheckerProcess() {}

protected:
  void initialize() override;
  void finalize() override;

private:
  void performCheck();
  void scheduleNext(const Duration& duration);
  void processCheckResult(
      const Stopwatch& stopwatch,
      const Result<CheckStatusInfo>& result);

  process::Future<int> commandCheck();

  std::string wrapDockerExec(const CommandInfo& command);

  process::Future<int> nestedCommandCheck();
  void _nestedCommandCheck(std::shared_ptr<process::Promise<int>> promise);
  void __nestedCommandCheck(
      std::shared_ptr<process::Promise<int>> promise,
      process::http::Connection connection);
  void ___nestedCommandCheck(
      std::shared_ptr<process::Promise<int>> promise,
      const ContainerID& checkContainerId,
      const process::http::Response& launchResponse);

  void nestedCommandCheckFailure(
      std::shared_ptr<process::Promise<int>> promise,
      process::http::Connection connection,
      const ContainerID& checkContainerId,
      std::shared_ptr<bool> checkTimedOut,
      const std::string& failure);

  process::Future<Option<int>> waitNestedContainer(
      const ContainerID& containerId);
  process::Future<Option<int>> _waitNestedContainer(
      const ContainerID& containerId,
      const process::http::Response& httpResponse);

  void processCommandCheckResult(
      const Stopwatch& stopwatch,
      const process::Future<int>& future);

#ifdef __WINDOWS__
  std::vector<std::string> dockerNetworkCmd(const DockerRuntimeInfo& info);
#endif // __WINDOWS__

  process::Future<int> httpCheck();
  std::vector<std::string> createHttpCheckCmd(const CheckInfo::Http& http);
  process::Future<int> _httpCheck(
      const std::tuple<process::Future<Option<int>>,
                       process::Future<std::string>,
                       process::Future<std::string>>& t);
  void processHttpCheckResult(
      const Stopwatch& stopwatch,
      const process::Future<int>& future);

  process::Future<bool> tcpCheck();
  std::vector<std::string> createTcpCheckCmd(const CheckInfo::Tcp& tcp);
  process::Future<bool> _tcpCheck(
      const std::tuple<process::Future<Option<int>>,
                       process::Future<std::string>,
                       process::Future<std::string>>& t);
  void processTcpCheckResult(
      const Stopwatch& stopwatch,
      const process::Future<bool>& future);

  const CheckInfo check;
  Duration checkDelay;
  Duration checkInterval;
  Duration checkTimeout;

  // Contains the binary for TCP checks.
  const std::string launcherDir;

  const lambda::function<void(const Try<CheckStatusInfo>&)> updateCallback;
  const TaskID taskId;
  const Option<pid_t> taskPid;
  const std::vector<std::string> namespaces;
  const Option<ContainerID> taskContainerId;
  const Option<process::http::URL> agentURL;
  const Option<std::string> authorizationHeader;
  const Option<std::string> scheme;
  const std::string name;
  const ContainerRuntimeInfo runtime;
  const bool commandCheckViaAgent;

  // If set to true, the TCP/HTTP(S) check will be performed over IPv6,
  // otherwise, it will be performed over IPv4.
  //
  // TODO(qianzhang): Once we figure out how the IPv4/IPv6 should be supported
  // in the health check API (i.e., the `CheckInfo` protobuf message), we may
  // consider to remove this field which is a temporary solution for now.
  const bool ipv6;

  Option<lambda::function<pid_t(const lambda::function<int()>&)>> clone;

  bool paused;

  // Contains the ID of the most recently terminated nested container
  // that was used to perform a COMMAND check.
  Option<ContainerID> previousCheckContainerId;
};

} // namespace checks {
} // namespace internal {
} // namespace mesos {

#endif // __CHECKER_PROCESS_HPP__
