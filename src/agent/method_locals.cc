/**
 * Copyright 2015 Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "method_locals.h"

#include "jni_utils.h"
#include "jvm_local_variable_reader.h"
#include "jvmti_buffer.h"

namespace devtools {
namespace cdbg {

MethodLocals::MethodLocals(
    LocalVariablesVisibilityPolicy* local_variables_visibility_policy)
    : local_variables_visibility_policy_(local_variables_visibility_policy) {
}


// Note: JNIEnv* is not available through jni() call.
void MethodLocals::JvmtiOnCompiledMethodUnload(jmethodID method) {
  method_vars_.erase(method);
}


std::shared_ptr<const MethodLocals::Entry> MethodLocals::GetLocalVariables(
    jmethodID method) {
  // Case 1: the local variables table is cached for "method".
  {
    MutexLock reader_lock(&mu_);

    auto it_method_vars = method_vars_.find(method);
    if (it_method_vars != method_vars_.end()) {
      return it_method_vars->second;
    }
  }

  // Case 2: we need to obtain the local variables table.
  {
    std::shared_ptr<Entry> locals = LoadEntry(method);
    if (locals == nullptr) {  // Failure
      return std::shared_ptr<Entry>(new Entry);
    }

    MutexLock writer_lock(&mu_);

    auto in = method_vars_.insert(
        std::make_pair(method, std::move(locals)));

    return in.first->second;
  }
}


std::shared_ptr<MethodLocals::Entry> MethodLocals::LoadEntry(
    jmethodID method) {
  jvmtiError err = JVMTI_ERROR_NONE;
  std::shared_ptr<Entry> entry(new Entry);

  // Fetch the class in which the method is defined.
  jclass cls = nullptr;
  err = jvmti()->GetMethodDeclaringClass(method, &cls);
  if (err != JVMTI_ERROR_NONE) {
    LOG(ERROR) << "GetMethodDeclaringClass failed, error: " << err;
    return nullptr;  // Retry the operation in the future.
  }

  JniLocalRef auto_cls(cls);

  // Load information about local instance (i.e. "this" pointer).
  entry->local_instance = LoadLocalInstance(cls, method);

  if (local_variables_visibility_policy_ &&
      !local_variables_visibility_policy_->IsLocalVariablesDebuggerVisible(
          cls,
          method)) {
    // The policy for this method is not to show local variables.
    return entry;
  }

  // Load information about local variables.
  jint num_entries = 0;
  JvmtiBuffer<jvmtiLocalVariableEntry> table;
  err = jvmti()->GetLocalVariableTable(
      method,
      &num_entries,
      table.ref());
  if ((err != JVMTI_ERROR_NONE) &&
      (err != JVMTI_ERROR_ABSENT_INFORMATION) &&
      (err != JVMTI_ERROR_NATIVE_METHOD)) {
    LOG(ERROR) << "Local variables table is not available, error: " << err;
    return nullptr;  // Retry the operation in the future.
  }

  // The class doesn't contain debugging information or it's a JNI method.
  // Nevertheless we still want to insert it into the map, so that we don't
  // need to repeat call to GetLocalVariableTable again for this method.
  if (err != JVMTI_ERROR_NONE) {
    return entry;  // Return an entry without local variables.
  }

  // Figure out how many slots are used for arguments. This is to distinguish
  // between arguments and local variables.
  jint arguments_size = 0;
  if (num_entries > 0) {
    err = jvmti()->GetArgumentsSize(method, &arguments_size);
    if (err != JVMTI_ERROR_NONE) {
      LOG(ERROR) << "GetArgumentsSize failed, err = " << err
                 << ", assuming all entires are locals";
      arguments_size = 0;
    }
  }

  for (int i = 0; i < num_entries; ++i) {
    const jvmtiLocalVariableEntry& local_variable_entry = table.get()[i];
    entry->locals.push_back(
        std::unique_ptr<LocalVariableReader>(
            new JvmLocalVariableReader(
                local_variable_entry,
                local_variable_entry.slot < arguments_size)));
  }

  return entry;
}


std::unique_ptr<LocalVariableReader> MethodLocals::LoadLocalInstance(
    jclass cls,
    jmethodID method) {
  jvmtiError err = JVMTI_ERROR_NONE;

  // Ignore static methods.
  jint method_modifiers = 0;
  err = jvmti()->GetMethodModifiers(method, &method_modifiers);
  if (err != JVMTI_ERROR_NONE) {
    LOG(ERROR) << "GetMethodModifiers failed, error: " << err;
    return nullptr;
  }

  if ((method_modifiers & JVM_ACC_STATIC) != 0) {
    return nullptr;  // Local instance not available for static methods.
  }

  JvmtiBuffer<char> class_signature;
  JvmtiBuffer<char> class_generic;
  err = jvmti()->GetClassSignature(
      cls,
      class_signature.ref(),
      class_generic.ref());
  if (err != JVMTI_ERROR_NONE) {
    LOG(ERROR) << "GetClassSignature failed, error: " << err;
    return nullptr;
  }

  // JVMTI has two APIs to access local instance (i.e. "this" reference). The
  // one we are using here is "GetLocalObject". This is the same function used
  // to read local variables of object type. According to JVMTI specification,
  // local instance always has slot 0. It is also available throughout the
  // entire function, hence "start_location" and "length" span.
  // The alternative method to access local instance is through JVMTI
  // "GetLocalInstance" method. It is a better way than "GetLocalObject", but
  // only available in JRE7.
  // TODO(vlif): replace with dedicated class to call "GetLocalInstance" after
  // default JDK in Google switches to JDK7.
  jvmtiLocalVariableEntry localInstance;
  memset(&localInstance, 0, sizeof(localInstance));
  localInstance.start_location = 0;
  localInstance.length = -1;  // The local variable is available everywhere.
  localInstance.name = const_cast<char*>("this");
  localInstance.signature = const_cast<char*>(class_signature.get());
  localInstance.generic_signature = const_cast<char*>(class_generic.get());
  localInstance.slot = 0;

  // Marking local instance as an argument (rather than a local variable).
  return std::unique_ptr<LocalVariableReader>(
      new JvmLocalVariableReader(localInstance, true));
}


}  // namespace cdbg
}  // namespace devtools
