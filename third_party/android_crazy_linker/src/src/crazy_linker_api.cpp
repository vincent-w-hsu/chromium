// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implements the crazy linker C-based API exposed by <crazy_linker.h>

#include <crazy_linker.h>

#include <string.h>

#include "crazy_linker_ashmem.h"
#include "crazy_linker_error.h"
#include "crazy_linker_globals.h"
#include "crazy_linker_library_view.h"
#include "crazy_linker_proc_maps.h"
#include "crazy_linker_search_path_list.h"
#include "crazy_linker_shared_library.h"
#include "crazy_linker_system.h"
#include "crazy_linker_thread_data.h"
#include "crazy_linker_util.h"

using crazy::Globals;
using crazy::Error;
using crazy::RDebug;
using crazy::SearchPathList;
using crazy::ScopedLockedGlobals;
using crazy::LibraryView;

//
// crazy_context_t
//

struct crazy_context_t {
  size_t load_address = 0;
  Error error;
};

//
// API functions
//

extern "C" {

void crazy_set_sdk_build_version(int sdk_build_version) {
  // NOTE: This must be called before creating the Globals instance,
  // so do not use Globals::Get() or a ScopedLockedGlobals instance here.
  Globals::sdk_build_version = sdk_build_version;
}

crazy_context_t* crazy_context_create() {
  return new crazy_context_t();
}

const char* crazy_context_get_error(crazy_context_t* context) {
  const char* error = context->error.c_str();
  return (error[0] != '\0') ? error : NULL;
}

// Clear error in a given context.
void crazy_context_clear_error(crazy_context_t* context) {
  context->error = "";
}

void crazy_context_set_load_address(crazy_context_t* context,
                                    size_t load_address) {
  context->load_address = load_address;
}

size_t crazy_context_get_load_address(crazy_context_t* context) {
  return context->load_address;
}

void crazy_context_destroy(crazy_context_t* context) {
  delete context;
}

void crazy_set_java_vm(void* java_vm, int minimum_jni_version) {
  ScopedLockedGlobals globals;
  globals->InitJavaVm(java_vm, minimum_jni_version);
}

void crazy_get_java_vm(void** java_vm, int* minimum_jni_version) {
  ScopedLockedGlobals globals;
  *java_vm = globals->java_vm();
  *minimum_jni_version = globals->minimum_jni_version();
}

crazy_status_t crazy_add_search_path(const char* file_path) {
  ScopedLockedGlobals globals;
  globals->search_path_list()->AddPaths(file_path);
  return CRAZY_STATUS_SUCCESS;
}

crazy_status_t crazy_add_search_path_for_address(void* address) {
  uintptr_t load_address;
  char path[512];
  char* p;

  if (crazy::FindElfBinaryForAddress(
          address, &load_address, path, sizeof(path)) &&
      (p = strrchr(path, '/')) != NULL && p[1]) {
    *p = '\0';
    return crazy_add_search_path(path);
  }

  return CRAZY_STATUS_FAILURE;
}

void crazy_reset_search_paths(void) {
  ScopedLockedGlobals globals;
  globals->search_path_list()->ResetFromEnv("LD_LIBRARY_PATH");
}

crazy_status_t crazy_library_open(crazy_library_t** library,
                                  const char* lib_name,
                                  crazy_context_t* context) {
  ScopedLockedGlobals globals;
  LibraryView* view = globals->libraries()->LoadLibrary(
      lib_name, context->load_address, globals->search_path_list(),
      &context->error);

  if (!view)
    return CRAZY_STATUS_FAILURE;

  *library = reinterpret_cast<crazy_library_t*>(view);
  return CRAZY_STATUS_SUCCESS;
}

crazy_status_t crazy_library_get_info(crazy_library_t* library,
                                      crazy_context_t* context,
                                      crazy_library_info_t* info) {
  if (!library) {
    context->error = "Invalid library file handle";
    return CRAZY_STATUS_FAILURE;
  }

  LibraryView* wrap = reinterpret_cast<LibraryView*>(library);
  if (!wrap->GetInfo(&info->load_address,
                     &info->load_size,
                     &info->relro_start,
                     &info->relro_size,
                     &context->error)) {
    return CRAZY_STATUS_FAILURE;
  }

  return CRAZY_STATUS_SUCCESS;
}

crazy_status_t crazy_library_create_shared_relro(crazy_library_t* library,
                                                 crazy_context_t* context,
                                                 size_t load_address,
                                                 size_t* relro_start,
                                                 size_t* relro_size,
                                                 int* relro_fd) {
  LibraryView* wrap = reinterpret_cast<LibraryView*>(library);

  if (!library || !wrap->IsCrazy()) {
    context->error = "Invalid library file handle";
    return CRAZY_STATUS_FAILURE;
  }

  crazy::SharedLibrary* lib = wrap->GetCrazy();
  if (!lib->CreateSharedRelro(
           load_address, relro_start, relro_size, relro_fd, &context->error))
    return CRAZY_STATUS_FAILURE;

  return CRAZY_STATUS_SUCCESS;
}

crazy_status_t crazy_library_use_shared_relro(crazy_library_t* library,
                                              crazy_context_t* context,
                                              size_t relro_start,
                                              size_t relro_size,
                                              int relro_fd) {
  LibraryView* wrap = reinterpret_cast<LibraryView*>(library);

  if (!library || !wrap->IsCrazy()) {
    context->error = "Invalid library file handle";
    return CRAZY_STATUS_FAILURE;
  }

  crazy::SharedLibrary* lib = wrap->GetCrazy();
  if (!lib->UseSharedRelro(relro_start, relro_size, relro_fd, &context->error))
    return CRAZY_STATUS_FAILURE;

  return CRAZY_STATUS_SUCCESS;
}

crazy_status_t crazy_library_find_by_name(const char* library_name,
                                          crazy_library_t** library) {
  {
    ScopedLockedGlobals globals;
    LibraryView* wrap = globals->libraries()->FindLibraryByName(library_name);
    if (!wrap)
      return CRAZY_STATUS_FAILURE;

    wrap->AddRef();
    *library = reinterpret_cast<crazy_library_t*>(wrap);
  }
  return CRAZY_STATUS_SUCCESS;
}

crazy_status_t crazy_library_find_symbol(crazy_library_t* library,
                                         const char* symbol_name,
                                         void** symbol_address) {
  LibraryView* wrap = reinterpret_cast<LibraryView*>(library);

  // TODO(digit): Handle NULL symbols properly.
  LibraryView::SearchResult sym = wrap->LookupSymbol(symbol_name);
  *symbol_address = sym.address;
  return sym.IsValid() ? CRAZY_STATUS_SUCCESS : CRAZY_STATUS_FAILURE;
}

crazy_status_t crazy_linker_find_symbol(const char* symbol_name,
                                        void** symbol_address) {
  // TODO(digit): Implement this.
  return CRAZY_STATUS_FAILURE;
}

crazy_status_t crazy_library_find_from_address(void* address,
                                               crazy_library_t** library) {
  {
    ScopedLockedGlobals globals;
    LibraryView* wrap = globals->libraries()->FindLibraryForAddress(address);
    if (!wrap)
      return CRAZY_STATUS_FAILURE;

    wrap->AddRef();

    *library = reinterpret_cast<crazy_library_t*>(wrap);
    return CRAZY_STATUS_SUCCESS;
  }
}

void crazy_library_close(crazy_library_t* library) {
  crazy_library_close_with_context(library, NULL);
}

void crazy_library_close_with_context(crazy_library_t* library,
                                      crazy_context_t* context) {
  if (library) {
    ScopedLockedGlobals globals;
    LibraryView* wrap = reinterpret_cast<LibraryView*>(library);

    globals->libraries()->UnloadLibrary(wrap);
  }
}

}  // extern "C"
