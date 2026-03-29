#pragma once

#ifdef WITH_AVISYNTH

#include "avisynth_legacy_path.h"

#include <avisynth.h>

#include <libaegisub/fs_fwd.h>

#include <initializer_list>
#include <vector>

namespace avisynth {
namespace detail {
	inline AVSValue InvokePathFunctionImpl(
		IScriptEnvironment *env,
		char const *function_name,
		std::string const& path_text,
		std::initializer_list<AVSValue> extra_args,
		std::initializer_list<char const *> extra_arg_names,
		bool append_utf8_flag)
	{
		std::vector<AVSValue> args;
		args.reserve(1 + extra_args.size() + (append_utf8_flag ? 1 : 0));
		args.emplace_back(env->SaveString(path_text.c_str()));
		for (auto const& arg : extra_args)
			args.push_back(arg);
		if (append_utf8_flag)
			args.emplace_back(true);

		std::vector<char const *> arg_names;
		if (!extra_arg_names.size() && !append_utf8_flag)
			return env->Invoke(function_name, AVSValue(args.data(), static_cast<int>(args.size())));

		arg_names.reserve(args.size());
		arg_names.push_back(nullptr);
		if (extra_arg_names.size()) {
			for (auto const *name : extra_arg_names)
				arg_names.push_back(name);
		}
		else {
			for (size_t i = 0; i < extra_args.size(); ++i)
				arg_names.push_back(nullptr);
		}
		if (append_utf8_flag)
			arg_names.push_back("utf8");

		return env->Invoke(function_name, AVSValue(args.data(), static_cast<int>(args.size())), arg_names.data());
	}
}

	inline AVSValue InvokeLegacyPathFunction(
		IScriptEnvironment *env,
		char const *function_name,
		agi::fs::path const& path,
		std::initializer_list<AVSValue> extra_args = {},
		std::initializer_list<char const *> extra_arg_names = {})
	{
		auto legacy_path = TryGetLegacyPathString(path);
		if (!legacy_path) {
			auto message = BuildLegacyPathFailureMessage(function_name, path);
			env->ThrowError("%s", message.c_str());
		}

		return detail::InvokePathFunctionImpl(env, function_name, *legacy_path, extra_args, extra_arg_names, false);
	}

	inline AVSValue InvokeUtf8PathFunction(
		IScriptEnvironment *env,
		char const *function_name,
		agi::fs::path const& path,
		std::initializer_list<AVSValue> extra_args = {},
		std::initializer_list<char const *> extra_arg_names = {})
	{
		try {
			return detail::InvokePathFunctionImpl(env, function_name, agi::fs::PathToString(path), extra_args, extra_arg_names, true);
		}
		catch (AvisynthError const&) {
			return InvokeLegacyPathFunction(env, function_name, path, extra_args, extra_arg_names);
		}
	}

	inline AVSValue InvokeUtf8BytesPathFunction(
		IScriptEnvironment *env,
		char const *function_name,
		agi::fs::path const& path,
		std::initializer_list<AVSValue> extra_args = {},
		std::initializer_list<char const *> extra_arg_names = {})
	{
		try {
			return detail::InvokePathFunctionImpl(env, function_name, agi::fs::PathToString(path), extra_args, extra_arg_names, false);
		}
		catch (AvisynthError const&) {
			return InvokeLegacyPathFunction(env, function_name, path, extra_args, extra_arg_names);
		}
	}
}

#endif
