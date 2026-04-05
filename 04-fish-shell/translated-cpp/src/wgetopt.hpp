/// Wide-character getopt implementation.
/// Transpiled from: fish-shell src/wgetopt.rs (tag 4.0.0)
///
/// Key translation decisions:
///   Rust enum Ordering { RequireOrder, Permute, ReturnInOrder }
///     -> C++ enum class Ordering
///   Rust enum ArgType { NoArgument, RequiredArgument, OptionalArgument }
///     -> C++ enum class ArgType
///   Rust WOption<'a> { name: &wstr, arg_type: ArgType, val: char }
///     -> C++ struct WOption { const wchar_t* name; ArgType arg_type; wchar_t val; }
///   Rust &'argarray mut [&'args wstr] (mutable slice of string refs)
///     -> C++ const wchar_t** argv + int argc
///   Rust Option<char> return -> int return (EOF = no more options)
///   Rust rotate_left on slice -> std::rotate
///   Rust remaining_text: &'args wstr -> const wchar_t* nextchar
///   Rust LongOptMatch enum -> local find_matching_long_opt with out params

#pragma once

#include <cassert>
#include <cstddef>
#include <cwchar>

/// Special char returned with ReturnInOrder ordering for non-option arguments.
inline constexpr wchar_t NON_OPTION_CHAR = L'\x01';

/// Whether an option takes an argument.
enum class ArgType { NoArgument, RequiredArgument, OptionalArgument };

/// Describes a long-named option.
struct WOption {
    const wchar_t* name = nullptr;
    ArgType arg_type = ArgType::NoArgument;
    wchar_t val = L'\0';

    constexpr WOption() = default;
    constexpr WOption(const wchar_t* n, ArgType a, wchar_t v)
        : name(n), arg_type(a), val(v) {}
};

/// Instanced wide-character getopt.
///
/// Modifies the order of elements in argv but not their contents.
class WGetopter {
public:
    using string_array_t = const wchar_t**;

    WGetopter() = default;

    /// Main entry point. Returns the next option character, or EOF when done.
    /// If longind is non-null, it receives the index of a matched long option.
    int wgetopt_long(int argc, string_array_t argv, const wchar_t* options,
                     const WOption* long_options, int* longind);

    // ---- Public state (matches GNU getopt interface) ----

    /// Argument to the current option, if any.
    const wchar_t* woptarg = nullptr;

    /// Index of the next element to scan.
    int woptind = 0;

    /// Set to unrecognized option character on '?' return.
    wchar_t woptopt = L'?';

    /// After scanning, first_nonopt..last_nonopt are the non-option elements.
    int first_nonopt = 0;
    int last_nonopt = 0;

private:
    enum class Ordering { RequireOrder, Permute, ReturnInOrder };

    enum class NextArgv { Finished, UnpermutedNonOption, FoundOption };

    const wchar_t* shortopts = nullptr;
    const wchar_t* nextchar = nullptr;
    Ordering ordering = Ordering::Permute;
    bool initialized = false;
    bool return_colon = false;

    void initialize(const wchar_t* optstring);
    void exchange(string_array_t argv);
    NextArgv advance_to_next_argv(int argc, string_array_t argv,
                                  const WOption* longopts);
    int handle_short_opt(int argc, string_array_t argv);
    void update_long_opt(int argc, string_array_t argv, const WOption* found,
                         size_t name_end, int* longind, int option_index,
                         int* retval);
    const WOption* find_matching_long_opt(const WOption* longopts,
                                          size_t name_end, int* exact,
                                          int* ambig, int* indfound) const;
    bool handle_long_opt(int argc, string_array_t argv,
                         const WOption* longopts, int* longind, int* retval);

    int wgetopt_inner(int argc, string_array_t argv, const wchar_t* optstring,
                      const WOption* longopts, int* longind);
};
