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

#include "neug/compiler/gopt/g_catalog.h"
#include "neug/compiler/main/metadata_manager.h"

namespace neug {
namespace main {

class MetadataRegistry {
 private:
  // MetadataManger is a single instance, there will be only one instance of
  // MetadataManager in the lifetime of the database.
  static thread_local MetadataManager* metadataManager;

 public:
  MetadataRegistry() = delete;

  static void registerMetadata(main::MetadataManager* metadataManager);

  static MetadataManager* getMetadata();

  /// Returns the currently registered MetadataManager without throwing.
  /// Returns nullptr if none is registered.  Useful for save/restore
  /// when multiple NeugDB instances are created and destroyed within
  /// the same process (e.g. Python-embedded mode).
  static MetadataManager* getMetadataQuietly() { return metadataManager; }

  /// Clears the current registration so the caller can detect a missing
  /// manager via getMetadataQuietly().
  static void clearMetadata() { metadataManager = nullptr; }

  static catalog::GCatalog* getCatalog();

  static neug::fsys::FileSystemRegistry* getVFS();
};

// Bind legacy metadata lookups to the database of the current operation,
// rather than whichever database happened to be opened most recently.
class ScopedMetadata {
 public:
  explicit ScopedMetadata(MetadataManager* value)
      : previous_(MetadataRegistry::getMetadataQuietly()) {
    if (value) MetadataRegistry::registerMetadata(value);
  }
  ~ScopedMetadata() {
    if (previous_) MetadataRegistry::registerMetadata(previous_);
    else MetadataRegistry::clearMetadata();
  }
  ScopedMetadata(const ScopedMetadata&) = delete;
  ScopedMetadata& operator=(const ScopedMetadata&) = delete;
 private:
  MetadataManager* previous_;
};
}  // namespace main
}  // namespace neug
