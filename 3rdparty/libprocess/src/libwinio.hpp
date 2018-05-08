// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __LIBWINIO_HPP__
#define __LIBWINIO_HPP__

#include "libwinio_impl.hpp"

namespace process {

// Windows event loop variable.
extern windows::EventLoop* libwinio_loop;

} // namespace process {

#endif // __LIBWINIO_HPP__
