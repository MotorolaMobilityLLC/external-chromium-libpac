// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "proxy_resolver_v8.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#include <utils/String8.h>
#include <v8.h>
#include <libplatform/libplatform.h>
#include <vector>

#include "net_util.h"
#include "proxy_resolver_script.h"

// Notes on the javascript environment:
//
// For the majority of the PAC utility functions, we use the same code
// as Firefox. See the javascript library that proxy_resolver_scipt.h
// pulls in.
//
// In addition, we implement a subset of Microsoft's extensions to PAC.
// - myIpAddressEx()
// - dnsResolveEx()
// - isResolvableEx()
// - isInNetEx()
// - sortIpAddressList()
//
// It is worth noting that the original PAC specification does not describe
// the return values on failure. Consequently, there are compatibility
// differences between browsers on what to return on failure, which are
// illustrated below:
//
// --------------------+-------------+-------------------+--------------
//                     | Firefox3    | InternetExplorer8 |  --> Us <---
// --------------------+-------------+-------------------+--------------
// myIpAddress()       | "127.0.0.1" |  ???              |  "127.0.0.1"
// dnsResolve()        | null        |  false            |  null
// myIpAddressEx()     | N/A         |  ""               |  ""
// sortIpAddressList() | N/A         |  false            |  false
// dnsResolveEx()      | N/A         |  ""               |  ""
// isInNetEx()         | N/A         |  false            |  false
// --------------------+-------------+-------------------+--------------
//
// TODO: The cell above reading ??? means I didn't test it.
//
// Another difference is in how dnsResolve() and myIpAddress() are
// implemented -- whether they should restrict to IPv4 results, or
// include both IPv4 and IPv6. The following table illustrates the
// differences:
//
// --------------------+-------------+-------------------+--------------
//                     | Firefox3    | InternetExplorer8 |  --> Us <---
// --------------------+-------------+-------------------+--------------
// myIpAddress()       | IPv4/IPv6   |  IPv4             |  IPv4
// dnsResolve()        | IPv4/IPv6   |  IPv4             |  IPv4
// isResolvable()      | IPv4/IPv6   |  IPv4             |  IPv4
// myIpAddressEx()     | N/A         |  IPv4/IPv6        |  IPv4/IPv6
// dnsResolveEx()      | N/A         |  IPv4/IPv6        |  IPv4/IPv6
// sortIpAddressList() | N/A         |  IPv4/IPv6        |  IPv4/IPv6
// isResolvableEx()    | N/A         |  IPv4/IPv6        |  IPv4/IPv6
// isInNetEx()         | N/A         |  IPv4/IPv6        |  IPv4/IPv6
// -----------------+-------------+-------------------+--------------

static bool DoIsStringASCII(const android::String16& str) {
  for (size_t i = 0; i < str.size(); i++) {
    unsigned short c = str.string()[i];
    if (c > 0x7F)
      return false;
  }
  return true;
}

bool IsStringASCII(const android::String16& str) {
  return DoIsStringASCII(str);
}

namespace net {

namespace {

// Pseudo-name for the PAC script.
const char kPacResourceName[] = "proxy-pac-script.js";
// Pseudo-name for the PAC utility script.
const char kPacUtilityResourceName[] = "proxy-pac-utility-script.js";

// External string wrapper so V8 can access the UTF16 string wrapped by
// ProxyResolverScriptData.
class V8ExternalStringFromScriptData
    : public v8::String::ExternalStringResource {
 public:
  explicit V8ExternalStringFromScriptData(
      const android::String16& script_data)
      : script_data_(script_data) {}

  virtual const uint16_t* data() const {
    return reinterpret_cast<const uint16_t*>(script_data_.string());
  }

  virtual size_t length() const {
    return script_data_.size();
  }

 private:
  const android::String16& script_data_;
//  DISALLOW_COPY_AND_ASSIGN(V8ExternalStringFromScriptData);
};

// External string wrapper so V8 can access a string literal.
class V8ExternalASCIILiteral
    : public v8::String::ExternalOneByteStringResource {
 public:
  // |ascii| must be a NULL-terminated C string, and must remain valid
  // throughout this object's lifetime.
  V8ExternalASCIILiteral(const char* ascii, size_t length)
      : ascii_(ascii), length_(length) {
  }

  virtual const char* data() const {
    return ascii_;
  }

  virtual size_t length() const {
    return length_;
  }

 private:
  const char* ascii_;
  size_t length_;
};

// When creating a v8::String from a C++ string we have two choices: create
// a copy, or create a wrapper that shares the same underlying storage.
// For small strings it is better to just make a copy, whereas for large
// strings there are savings by sharing the storage. This number identifies
// the cutoff length for when to start wrapping rather than creating copies.
const size_t kMaxStringBytesForCopy = 256;

template <class string_type>
inline typename string_type::value_type* WriteInto(string_type* str,
                                                   size_t length_with_null) {
  str->reserve(length_with_null);
  str->resize(length_with_null - 1);
  return &((*str)[0]);
}

// Converts a V8 String to a UTF8 std::string.
std::string V8StringToUTF8(v8::Handle<v8::String> s) {
  std::string result;
  s->WriteUtf8(WriteInto(&result, s->Length() + 1));
  return result;
}

// Converts a V8 String to a UTF16 string.
android::String16 V8StringToUTF16(v8::Handle<v8::String> s) {
  int len = s->Length();
  char16_t* buf = new char16_t[len + 1];
  s->Write(reinterpret_cast<uint16_t*>(buf), 0, len);
  android::String16 ret(buf, len);
  delete[] buf;
  return ret;
}

std::string UTF16ToASCII(const android::String16& str) {
  android::String8 rstr(str);
  return std::string(rstr.string());
}

// Converts an ASCII std::string to a V8 string.
v8::Local<v8::String> ASCIIStringToV8String(v8::Isolate* isolate, const std::string& s) {
  return v8::String::NewFromUtf8(isolate, s.data(), v8::String::kNormalString, s.size());
}

v8::Local<v8::String> UTF16StringToV8String(v8::Isolate* isolate, const android::String16& s) {
  return v8::String::NewFromTwoByte(
      isolate, reinterpret_cast<const uint16_t*>(s.string()),
      v8::String::kNormalString, s.size());
}

// Converts an ASCII string literal to a V8 string.
v8::Local<v8::String> ASCIILiteralToV8String(v8::Isolate* isolate, const char* ascii) {
//  DCHECK(IsStringASCII(ascii));
  size_t length = strlen(ascii);
  if (length <= kMaxStringBytesForCopy)
    return v8::String::NewFromUtf8(isolate, ascii, v8::String::kNormalString, length);
  return v8::String::NewExternal(isolate, new V8ExternalASCIILiteral(ascii, length));
}

// Stringizes a V8 object by calling its toString() method. Returns true
// on success. This may fail if the toString() throws an exception.
bool V8ObjectToUTF16String(v8::Handle<v8::Value> object,
                           android::String16* utf16_result,
                           v8::Isolate* isolate) {
  if (object.IsEmpty())
    return false;

  v8::HandleScope scope(isolate);
  v8::Local<v8::String> str_object = object->ToString();
  if (str_object.IsEmpty())
    return false;
  *utf16_result = V8StringToUTF16(str_object);
  return true;
}

// Extracts an hostname argument from |args|. On success returns true
// and fills |*hostname| with the result.
bool GetHostnameArgument(const v8::FunctionCallbackInfo<v8::Value>& args, std::string* hostname) {
  // The first argument should be a string.
  if (args.Length() == 0 || args[0].IsEmpty() || !args[0]->IsString())
    return false;

  const android::String16 hostname_utf16 = V8StringToUTF16(args[0]->ToString());

  // If the hostname is already in ASCII, simply return it as is.
  if (IsStringASCII(hostname_utf16)) {
    *hostname = UTF16ToASCII(hostname_utf16);
    return true;
  }
  return false;
}

// Wrapper for passing around IP address strings and IPAddressNumber objects.
struct IPAddress {
  IPAddress(const std::string& ip_string, const IPAddressNumber& ip_number)
      : string_value(ip_string),
        ip_address_number(ip_number) {
  }

  // Used for sorting IP addresses in ascending order in SortIpAddressList().
  // IP6 addresses are placed ahead of IPv4 addresses.
  bool operator<(const IPAddress& rhs) const {
    const IPAddressNumber& ip1 = this->ip_address_number;
    const IPAddressNumber& ip2 = rhs.ip_address_number;
    if (ip1.size() != ip2.size())
      return ip1.size() > ip2.size();  // IPv6 before IPv4.
    return memcmp(&ip1[0], &ip2[0], ip1.size()) < 0;  // Ascending order.
  }

  std::string string_value;
  IPAddressNumber ip_address_number;
};

template<typename STR>
bool RemoveCharsT(const STR& input,
                  const typename STR::value_type remove_chars[],
                  STR* output) {
  bool removed = false;
  size_t found;

  *output = input;

  found = output->find_first_of(remove_chars);
  while (found != STR::npos) {
    removed = true;
    output->replace(found, 1, STR());
    found = output->find_first_of(remove_chars, found);
  }

  return removed;
}

bool RemoveChars(const std::string& input,
                 const char remove_chars[],
                 std::string* output) {
  return RemoveCharsT(input, remove_chars, output);
}

// Handler for "sortIpAddressList(IpAddressList)". |ip_address_list| is a
// semi-colon delimited string containing IP addresses.
// |sorted_ip_address_list| is the resulting list of sorted semi-colon delimited
// IP addresses or an empty string if unable to sort the IP address list.
// Returns 'true' if the sorting was successful, and 'false' if the input was an
// empty string, a string of separators (";" in this case), or if any of the IP
// addresses in the input list failed to parse.
bool SortIpAddressList(const std::string& ip_address_list,
                       std::string* sorted_ip_address_list) {
  sorted_ip_address_list->clear();

  // Strip all whitespace (mimics IE behavior).
  std::string cleaned_ip_address_list;
  RemoveChars(ip_address_list, " \t", &cleaned_ip_address_list);
  if (cleaned_ip_address_list.empty())
    return false;

  // Split-up IP addresses and store them in a vector.
  std::vector<IPAddress> ip_vector;
  IPAddressNumber ip_num;
  char *tok_list = strtok((char *)cleaned_ip_address_list.c_str(), ";");
  while (tok_list != NULL) {
    if (!ParseIPLiteralToNumber(tok_list, &ip_num))
      return false;
    ip_vector.push_back(IPAddress(tok_list, ip_num));
    tok_list = strtok(NULL, ";");
  }

  if (ip_vector.empty())  // Can happen if we have something like
    return false;         // sortIpAddressList(";") or sortIpAddressList("; ;")

  // Sort lists according to ascending numeric value.
  if (ip_vector.size() > 1)
    std::stable_sort(ip_vector.begin(), ip_vector.end());

  // Return a semi-colon delimited list of sorted addresses (IPv6 followed by
  // IPv4).
  for (size_t i = 0; i < ip_vector.size(); ++i) {
    if (i > 0)
      *sorted_ip_address_list += ";";
    *sorted_ip_address_list += ip_vector[i].string_value;
  }
  return true;
}


// Handler for "isInNetEx(ip_address, ip_prefix)". |ip_address| is a string
// containing an IPv4/IPv6 address, and |ip_prefix| is a string containg a
// slash-delimited IP prefix with the top 'n' bits specified in the bit
// field. This returns 'true' if the address is in the same subnet, and
// 'false' otherwise. Also returns 'false' if the prefix is in an incorrect
// format, or if an address and prefix of different types are used (e.g. IPv6
// address and IPv4 prefix).
bool IsInNetEx(const std::string& ip_address, const std::string& ip_prefix) {
  IPAddressNumber address;
  std::string cleaned_ip_address;
  if (RemoveChars(ip_address, " \t", &cleaned_ip_address))
    return false;
  if (!ParseIPLiteralToNumber(ip_address, &address))
    return false;

  IPAddressNumber prefix;
  size_t prefix_length_in_bits;
  if (!ParseCIDRBlock(ip_prefix, &prefix, &prefix_length_in_bits))
    return false;

  // Both |address| and |prefix| must be of the same type (IPv4 or IPv6).
  if (address.size() != prefix.size())
    return false;

  return IPNumberMatchesPrefix(address, prefix, prefix_length_in_bits);
}

}  // namespace

class ArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
  public:
   virtual void* Allocate(size_t length) {
     void* data = AllocateUninitialized(length);
     return data == NULL ? data : memset(data, 0, length);
   }
   virtual void* AllocateUninitialized(size_t length) { return malloc(length); }
   virtual void Free(void* data, size_t) { free(data); }
};


// ProxyResolverV8::Context ---------------------------------------------------

class ProxyResolverV8::Context {
 public:
  explicit Context(ProxyResolverJSBindings* js_bindings,
          ProxyErrorListener* error_listener, v8::Isolate* isolate)
      : js_bindings_(js_bindings), error_listener_(error_listener), isolate_(isolate) {
  }

  ~Context() {
    v8::Locker locked(isolate_);
    v8::Isolate::Scope isolate_scope(isolate_);

    v8_this_.Reset();
    v8_context_.Reset();
  }

  int ResolveProxy(const android::String16 url, const android::String16 host,
        android::String16* results) {
    v8::Locker locked(isolate_);
    v8::Isolate::Scope isolate_scope(isolate_);
    v8::HandleScope scope(isolate_);

    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate_, v8_context_);
    v8::Context::Scope function_scope(context);

    v8::Local<v8::Value> function;
    if (!GetFindProxyForURL(&function)) {
      error_listener_->ErrorMessage(
          android::String16("FindProxyForURL() is undefined"));
      return ERR_PAC_SCRIPT_FAILED;
    }

    v8::Handle<v8::Value> argv[] = {
        UTF16StringToV8String(isolate_, url),
        UTF16StringToV8String(isolate_, host) };

    v8::TryCatch try_catch;
    v8::Local<v8::Value> ret = v8::Function::Cast(*function)->Call(
        context->Global(), 2, argv);

    if (try_catch.HasCaught()) {
      error_listener_->ErrorMessage(
          V8StringToUTF16(try_catch.Message()->Get()));
      return ERR_PAC_SCRIPT_FAILED;
    }

    if (!ret->IsString()) {
      error_listener_->ErrorMessage(
          android::String16("FindProxyForURL() did not return a string."));
      return ERR_PAC_SCRIPT_FAILED;
    }

    *results = V8StringToUTF16(ret->ToString());

    if (!IsStringASCII(*results)) {
      // TODO:         Rather than failing when a wide string is returned, we
      //               could extend the parsing to handle IDNA hostnames by
      //               converting them to ASCII punycode.
      //               crbug.com/47234
      error_listener_->ErrorMessage(
          android::String16("FindProxyForURL() returned a non-ASCII string"));
      return ERR_PAC_SCRIPT_FAILED;
    }

    return OK;
  }

  int InitV8(const android::String16& pac_script) {
    v8::Locker locked(isolate_);
    v8::Isolate::Scope isolate_scope(isolate_);
    v8::HandleScope scope(isolate_);

    v8_this_.Reset(isolate_, v8::External::New(isolate_, this));
    v8::Local<v8::External> v8_this =
        v8::Local<v8::External>::New(isolate_, v8_this_);
    v8::Local<v8::ObjectTemplate> global_template = v8::ObjectTemplate::New();

    // Attach the javascript bindings.
    v8::Local<v8::FunctionTemplate> alert_template =
        v8::FunctionTemplate::New(isolate_, &AlertCallback, v8_this);
    global_template->Set(ASCIILiteralToV8String(isolate_, "alert"), alert_template);

    v8::Local<v8::FunctionTemplate> my_ip_address_template =
        v8::FunctionTemplate::New(isolate_, &MyIpAddressCallback, v8_this);
    global_template->Set(ASCIILiteralToV8String(isolate_, "myIpAddress"),
        my_ip_address_template);

    v8::Local<v8::FunctionTemplate> dns_resolve_template =
        v8::FunctionTemplate::New(isolate_, &DnsResolveCallback, v8_this);
    global_template->Set(ASCIILiteralToV8String(isolate_, "dnsResolve"),
        dns_resolve_template);

    // Microsoft's PAC extensions:

    v8::Local<v8::FunctionTemplate> dns_resolve_ex_template =
        v8::FunctionTemplate::New(isolate_, &DnsResolveExCallback, v8_this);
    global_template->Set(ASCIILiteralToV8String(isolate_, "dnsResolveEx"),
                         dns_resolve_ex_template);

    v8::Local<v8::FunctionTemplate> my_ip_address_ex_template =
        v8::FunctionTemplate::New(isolate_, &MyIpAddressExCallback, v8_this);
    global_template->Set(ASCIILiteralToV8String(isolate_, "myIpAddressEx"),
                         my_ip_address_ex_template);

    v8::Local<v8::FunctionTemplate> sort_ip_address_list_template =
        v8::FunctionTemplate::New(isolate_, &SortIpAddressListCallback, v8_this);
    global_template->Set(ASCIILiteralToV8String(isolate_, "sortIpAddressList"),
                         sort_ip_address_list_template);

    v8::Local<v8::FunctionTemplate> is_in_net_ex_template =
        v8::FunctionTemplate::New(isolate_, &IsInNetExCallback, v8_this);
    global_template->Set(ASCIILiteralToV8String(isolate_, "isInNetEx"),
                         is_in_net_ex_template);

    v8_context_.Reset(
        isolate_, v8::Context::New(isolate_, NULL, global_template));

    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate_, v8_context_);
    v8::Context::Scope ctx(context);

    // Add the PAC utility functions to the environment.
    // (This script should never fail, as it is a string literal!)
    // Note that the two string literals are concatenated.
    int rv = RunScript(
        ASCIILiteralToV8String(isolate_,
            PROXY_RESOLVER_SCRIPT
            PROXY_RESOLVER_SCRIPT_EX),
        kPacUtilityResourceName);
    if (rv != OK) {
      return rv;
    }

    // Add the user's PAC code to the environment.
    rv = RunScript(UTF16StringToV8String(isolate_, pac_script), kPacResourceName);
    if (rv != OK) {
      return rv;
    }

    // At a minimum, the FindProxyForURL() function must be defined for this
    // to be a legitimiate PAC script.
    v8::Local<v8::Value> function;
    if (!GetFindProxyForURL(&function))
      return ERR_PAC_SCRIPT_FAILED;

    return OK;
  }

  void PurgeMemory() {
    v8::Locker locked(isolate_);
    v8::Isolate::Scope isolate_scope(isolate_);
    isolate_->LowMemoryNotification();
  }

 private:
  bool GetFindProxyForURL(v8::Local<v8::Value>* function) {
    v8::Local<v8::Context> context =
        v8::Local<v8::Context>::New(isolate_, v8_context_);
    *function = context->Global()->Get(
        ASCIILiteralToV8String(isolate_, "FindProxyForURL"));
    return (*function)->IsFunction();
  }

  // Handle an exception thrown by V8.
  void HandleError(v8::Handle<v8::Message> message) {
    if (message.IsEmpty())
      return;
    error_listener_->ErrorMessage(V8StringToUTF16(message->Get()));
  }

  // Compiles and runs |script| in the current V8 context.
  // Returns OK on success, otherwise an error code.
  int RunScript(v8::Handle<v8::String> script, const char* script_name) {
    v8::TryCatch try_catch;

    // Compile the script.
    v8::ScriptOrigin origin =
        v8::ScriptOrigin(ASCIILiteralToV8String(isolate_, script_name));
    v8::Local<v8::Script> code = v8::Script::Compile(script, &origin);

    // Execute.
    if (!code.IsEmpty())
      code->Run();

    // Check for errors.
    if (try_catch.HasCaught()) {
      HandleError(try_catch.Message());
      return ERR_PAC_SCRIPT_FAILED;
    }

    return OK;
  }

  // V8 callback for when "alert()" is invoked by the PAC script.
  static void AlertCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    Context* context =
        static_cast<Context*>(v8::External::Cast(*args.Data())->Value());

    // Like firefox we assume "undefined" if no argument was specified, and
    // disregard any arguments beyond the first.
    android::String16 message;
    if (args.Length() == 0) {
      std::string undef = "undefined";
      android::String8 undef8(undef.c_str());
      android::String16 wundef(undef8);
      message = wundef;
    } else {
      if (!V8ObjectToUTF16String(args[0], &message, args.GetIsolate()))
        return;  // toString() threw an exception.
    }

    context->error_listener_->AlertMessage(message);
    return;
  }

  // V8 callback for when "myIpAddress()" is invoked by the PAC script.
  static void MyIpAddressCallback(
      const v8::FunctionCallbackInfo<v8::Value>& args) {
    Context* context =
        static_cast<Context*>(v8::External::Cast(*args.Data())->Value());

    std::string result;
    bool success;

    {
      v8::Unlocker unlocker(args.GetIsolate());

      // We shouldn't be called with any arguments, but will not complain if
      // we are.
      success = context->js_bindings_->MyIpAddress(&result);
    }

    if (!success) {
      args.GetReturnValue().Set(ASCIILiteralToV8String(args.GetIsolate(), "127.0.0.1"));
    } else {
      args.GetReturnValue().Set(ASCIIStringToV8String(args.GetIsolate(), result));
    }
  }

  // V8 callback for when "myIpAddressEx()" is invoked by the PAC script.
  static void MyIpAddressExCallback(
      const v8::FunctionCallbackInfo<v8::Value>& args) {
    Context* context =
        static_cast<Context*>(v8::External::Cast(*args.Data())->Value());

    std::string ip_address_list;
    bool success;

    {
      v8::Unlocker unlocker(args.GetIsolate());

      // We shouldn't be called with any arguments, but will not complain if
      // we are.
      success = context->js_bindings_->MyIpAddressEx(&ip_address_list);
    }

    if (!success)
      ip_address_list = std::string();
    args.GetReturnValue().Set(ASCIIStringToV8String(args.GetIsolate(), ip_address_list));
  }

  // V8 callback for when "dnsResolve()" is invoked by the PAC script.
  static void DnsResolveCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    Context* context =
        static_cast<Context*>(v8::External::Cast(*args.Data())->Value());

    // We need at least one string argument.
    std::string hostname;
    if (!GetHostnameArgument(args, &hostname)) {
      return;
    }

    std::string ip_address;
    bool success;

    {
      v8::Unlocker unlocker(args.GetIsolate());
      success = context->js_bindings_->DnsResolve(hostname, &ip_address);
    }

    if (success) {
      args.GetReturnValue().Set(ASCIIStringToV8String(args.GetIsolate(), ip_address));
    } else {
      args.GetReturnValue().SetNull();
    }
  }

  // V8 callback for when "dnsResolveEx()" is invoked by the PAC script.
  static void DnsResolveExCallback(
      const v8::FunctionCallbackInfo<v8::Value>& args) {
    Context* context =
        static_cast<Context*>(v8::External::Cast(*args.Data())->Value());

    // We need at least one string argument.
    std::string hostname;
    if (!GetHostnameArgument(args, &hostname)) {
      args.GetReturnValue().SetNull();
      return;
    }

    std::string ip_address_list;
    bool success;

    {
      v8::Unlocker unlocker(args.GetIsolate());
      success = context->js_bindings_->DnsResolveEx(hostname, &ip_address_list);
    }

    if (!success)
      ip_address_list = std::string();

    args.GetReturnValue().Set(ASCIIStringToV8String(args.GetIsolate(), ip_address_list));
  }

  // V8 callback for when "sortIpAddressList()" is invoked by the PAC script.
  static void SortIpAddressListCallback(
      const v8::FunctionCallbackInfo<v8::Value>& args) {
    // We need at least one string argument.
    if (args.Length() == 0 || args[0].IsEmpty() || !args[0]->IsString()) {
      args.GetReturnValue().SetNull();
      return;
    }

    std::string ip_address_list = V8StringToUTF8(args[0]->ToString());
    std::string sorted_ip_address_list;
    bool success = SortIpAddressList(ip_address_list, &sorted_ip_address_list);
    if (!success) {
      args.GetReturnValue().Set(false);
      return;
    }
    args.GetReturnValue().Set(ASCIIStringToV8String(args.GetIsolate(), sorted_ip_address_list));
  }

  // V8 callback for when "isInNetEx()" is invoked by the PAC script.
  static void IsInNetExCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
    // We need at least 2 string arguments.
    if (args.Length() < 2 || args[0].IsEmpty() || !args[0]->IsString() ||
        args[1].IsEmpty() || !args[1]->IsString()) {
      args.GetReturnValue().SetNull();
      return;
    }

    std::string ip_address = V8StringToUTF8(args[0]->ToString());
    std::string ip_prefix = V8StringToUTF8(args[1]->ToString());
    args.GetReturnValue().Set(IsInNetEx(ip_address, ip_prefix));
  }

  ProxyResolverJSBindings* js_bindings_;
  ProxyErrorListener* error_listener_;
  v8::Isolate* isolate_;
  v8::Persistent<v8::External> v8_this_;
  v8::Persistent<v8::Context> v8_context_;
};

// ProxyResolverV8 ------------------------------------------------------------

bool ProxyResolverV8::initialized_for_this_process_ = false;

ProxyResolverV8::ProxyResolverV8(
    ProxyResolverJSBindings* custom_js_bindings,
    ProxyErrorListener* error_listener)
    : context_(NULL), js_bindings_(custom_js_bindings),
      error_listener_(error_listener) {
  if (!initialized_for_this_process_) {
    v8::Platform* platform = v8::platform::CreateDefaultPlatform();
    v8::V8::InitializePlatform(platform);
    v8::V8::Initialize();
    initialized_for_this_process_ = true;
  }
}

ProxyResolverV8::~ProxyResolverV8() {
  if (context_ != NULL) {
    delete context_;
    context_ = NULL;
  }
  if (js_bindings_ != NULL) {
    delete js_bindings_;
  }
}

int ProxyResolverV8::GetProxyForURL(const android::String16 spec, const android::String16 host,
                                    android::String16* results) {
  // If the V8 instance has not been initialized (either because
  // SetPacScript() wasn't called yet, or because it failed.
  if (context_ == NULL)
    return ERR_FAILED;

  // Otherwise call into V8.
  int rv = context_->ResolveProxy(spec, host, results);

  return rv;
}

void ProxyResolverV8::PurgeMemory() {
  context_->PurgeMemory();
}

int ProxyResolverV8::SetPacScript(const android::String16& script_data) {
  if (context_ != NULL) {
    delete context_;
    context_ = NULL;
  }
  if (script_data.size() == 0)
    return ERR_PAC_SCRIPT_FAILED;

  // Use the built-in locale-aware definitions instead of the ones provided by
  // ICU. This makes things like String.prototype.toUpperCase() not be
  // undefined.
  static const char kNoIcuCaseMapping[] = "--no-icu_case_mapping";
  v8::V8::SetFlagsFromString(kNoIcuCaseMapping, strlen(kNoIcuCaseMapping));

  // Try parsing the PAC script.
  ArrayBufferAllocator allocator;
  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator = &allocator;

  context_ = new Context(js_bindings_, error_listener_, v8::Isolate::New(create_params));
  int rv;
  if ((rv = context_->InitV8(script_data)) != OK) {
    context_ = NULL;
  }
  if (rv != OK)
    context_ = NULL;
  return rv;
}

}  // namespace net
