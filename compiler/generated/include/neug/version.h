/** Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

/**
 * @file version.h
 * @brief Version information for NeuG
 */

/// NeuG version string
#define NEUG_VERSION_STRING "0.1.0"

/// NeuG major version
#define NEUG_VERSION_MAJOR 0

/// NeuG minor version
#define NEUG_VERSION_MINOR 1

/// NeuG patch version
#define NEUG_VERSION_PATCH 0

/// NeuG version as integer (major * 10000 + minor * 100 + patch)
#define NEUG_VERSION_NUMBER                                      \
  (0 * 10000 + 1 * 100 + \
   0)
