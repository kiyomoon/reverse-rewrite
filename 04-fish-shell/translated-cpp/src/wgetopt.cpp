/// Wide-character getopt implementation.
/// Transpiled from: fish-shell src/wgetopt.rs (tag 4.0.0)
///
/// Originally derived from GNU getopt, adapted for wide characters.
/// The Rust version was a line-by-line port of the C++ version.
/// This transpilation reverses that port.
///
/// Key translation decisions:
///   Rust slice.rotate_left(n) -> std::rotate
///   Rust remaining_text.char_at(0) -> *nextchar
///   Rust remaining_text.is_empty() -> !nextchar || *nextchar == L'\0'
///   Rust Option<char> return -> int return (EOF for None)
///   Rust wstr.find(['=']) -> manual scan for '='
///   Rust shortopts.as_char_slice().contains(&c) -> wcschr(shortopts, c)

#include "wgetopt.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <cwchar>

// ---- exchange ----
// Swap two subsequences in argv:
//   [first_nonopt, last_nonopt) — non-options that were skipped
//   [last_nonopt, woptind) — options scanned after the non-options
// After exchange, options come first and non-options follow.
void WGetopter::exchange(string_array_t argv) {
    int left = first_nonopt;
    int middle = last_nonopt;
    int right = woptind;
    assert(left <= middle && middle <= right);

    // std::rotate to move the non-options to after the options.
    // This is equivalent to Rust's slice.rotate_left(middle - left).
    std::rotate(argv + left, argv + middle, argv + right);

    first_nonopt += (woptind - last_nonopt);
    last_nonopt = woptind;
}

// ---- initialize ----
void WGetopter::initialize(const wchar_t* optstring) {
    first_nonopt = last_nonopt = woptind = 1;
    nextchar = nullptr;

    if (optstring[0] == L'-') {
        ordering = Ordering::ReturnInOrder;
        ++optstring;
    } else if (optstring[0] == L'+') {
        ordering = Ordering::RequireOrder;
        ++optstring;
    } else {
        ordering = Ordering::Permute;
    }

    if (optstring[0] == L':') {
        return_colon = true;
        ++optstring;
    }

    shortopts = optstring;
    initialized = true;
}

// ---- advance_to_next_argv ----
WGetopter::NextArgv WGetopter::advance_to_next_argv(
    int argc, string_array_t argv, const WOption* longopts) {

    if (ordering == Ordering::Permute) {
        // Permute: if we've found options following non-options, exchange.
        if (first_nonopt != last_nonopt && last_nonopt != woptind) {
            exchange(argv);
        } else if (last_nonopt != woptind) {
            first_nonopt = woptind;
        }

        // Skip further non-options.
        while (woptind < argc &&
               (argv[woptind][0] != L'-' || argv[woptind][1] == L'\0')) {
            woptind++;
        }
        last_nonopt = woptind;
    }

    // Handle the special "--" element.
    if (woptind != argc && !std::wcscmp(argv[woptind], L"--")) {
        woptind++;

        if (first_nonopt != last_nonopt && last_nonopt != woptind) {
            exchange(argv);
        } else if (first_nonopt == last_nonopt) {
            first_nonopt = woptind;
        }

        last_nonopt = argc;
        woptind = argc;
    }

    // Done with all elements?
    if (woptind == argc) {
        if (first_nonopt != last_nonopt) {
            woptind = first_nonopt;
        }
        return NextArgv::Finished;
    }

    // Non-option found without permutation?
    if (argv[woptind][0] != L'-' || argv[woptind][1] == L'\0') {
        if (ordering == Ordering::RequireOrder) {
            return NextArgv::Finished;
        }
        woptarg = argv[woptind++];
        return NextArgv::UnpermutedNonOption;
    }

    // Found an option. Skip initial punctuation ('-' or '--').
    nextchar = argv[woptind] + 1 +
               (longopts != nullptr && argv[woptind][1] == L'-' ? 1 : 0);
    return NextArgv::FoundOption;
}

// ---- handle_short_opt ----
int WGetopter::handle_short_opt(int argc, string_array_t argv) {
    wchar_t c = *nextchar++;
    const wchar_t* temp = std::wcschr(shortopts, c);

    // Increment woptind when we exhaust the current element.
    if (*nextchar == L'\0') ++woptind;

    if (temp == nullptr || c == L':') {
        woptopt = c;
        if (*nextchar != L'\0') woptind++;
        return L'?';
    }

    if (temp[1] != L':') {
        return c;
    }

    if (temp[2] == L':') {
        // Optional argument.
        if (*nextchar != L'\0') {
            woptarg = nextchar;
            woptind++;
        } else {
            woptarg = nullptr;
        }
        nextchar = nullptr;
    } else {
        // Required argument.
        if (*nextchar != L'\0') {
            woptarg = nextchar;
            woptind++;
        } else if (woptind == argc) {
            woptopt = c;
            c = return_colon ? L':' : L'?';
        } else {
            woptarg = argv[woptind++];
        }
        nextchar = nullptr;
    }

    return c;
}

// ---- find_matching_long_opt ----
const WOption* WGetopter::find_matching_long_opt(
    const WOption* longopts, size_t name_end,
    int* exact, int* ambig, int* indfound) const {

    const WOption* found = nullptr;
    int option_index = 0;

    for (const WOption* p = longopts; p->name; ++p, ++option_index) {
        if (!std::wcsncmp(p->name, nextchar, name_end)) {
            if (name_end == std::wcslen(p->name)) {
                // Exact match.
                found = p;
                *indfound = option_index;
                *exact = 1;
                break;
            } else if (found == nullptr) {
                // First non-exact match.
                found = p;
                *indfound = option_index;
            } else {
                // Second or later non-exact match.
                *ambig = 1;
            }
        }
    }
    return found;
}

// ---- update_long_opt ----
void WGetopter::update_long_opt(
    int argc, string_array_t argv, const WOption* found,
    size_t name_end, int* longind, int option_index, int* retval) {

    woptind++;
    assert(nextchar[name_end] == L'\0' || nextchar[name_end] == L'=');

    if (nextchar[name_end] == L'=') {
        if (found->arg_type != ArgType::NoArgument) {
            woptarg = &nextchar[name_end + 1];
        } else {
            nextchar += std::wcslen(nextchar);
            *retval = L'?';
            return;
        }
    } else if (found->arg_type == ArgType::RequiredArgument) {
        if (woptind < argc) {
            woptarg = argv[woptind++];
        } else {
            nextchar += std::wcslen(nextchar);
            *retval = return_colon ? L':' : L'?';
            return;
        }
    }

    nextchar += std::wcslen(nextchar);
    if (longind != nullptr) *longind = option_index;
    *retval = found->val;
}

// ---- handle_long_opt ----
bool WGetopter::handle_long_opt(
    int argc, string_array_t argv, const WOption* longopts,
    int* longind, int* retval) {

    int exact = 0, ambig = 0, indfound = 0;

    size_t name_end = 0;
    while (nextchar[name_end] && nextchar[name_end] != L'=') {
        name_end++;
    }

    const WOption* found =
        find_matching_long_opt(longopts, name_end, &exact, &ambig, &indfound);

    if (ambig && !exact) {
        nextchar += std::wcslen(nextchar);
        woptind++;
        *retval = L'?';
        return true;
    }

    if (found) {
        update_long_opt(argc, argv, found, name_end, longind, indfound, retval);
        return true;
    }

    // Can't find as a long option. If it starts with '--' or isn't a valid
    // short option, it's an error. Otherwise try as a short option.
    if (argv[woptind][1] == L'-' ||
        std::wcschr(shortopts, *nextchar) == nullptr) {
        nextchar = L"";
        woptind++;
        *retval = L'?';
        return true;
    }

    return false;
}

// ---- wgetopt_inner ----
int WGetopter::wgetopt_inner(
    int argc, string_array_t argv, const wchar_t* optstring,
    const WOption* longopts, int* longind) {

    if (!initialized) initialize(optstring);

    woptarg = nullptr;

    if (nextchar == nullptr || *nextchar == L'\0') {
        NextArgv result = advance_to_next_argv(argc, argv, longopts);
        switch (result) {
            case NextArgv::UnpermutedNonOption:
                return NON_OPTION_CHAR;
            case NextArgv::Finished:
                return EOF;
            case NextArgv::FoundOption:
                break;
        }
    }

    // Try long options if applicable.
    if (longopts && woptind < argc) {
        const wchar_t* arg = argv[woptind];
        bool try_long = false;
        if (arg[0] == L'-' && arg[1] == L'-') {
            try_long = true; // --foo
        } else if (!std::wcschr(shortopts, arg[1])) {
            try_long = true; // -f where f is not a valid short opt
        }

        if (try_long) {
            int retval = 0;
            if (handle_long_opt(argc, argv, longopts, longind, &retval)) {
                return retval;
            }
        }
    }

    return handle_short_opt(argc, argv);
}

// ---- Public entry point ----
int WGetopter::wgetopt_long(int argc, string_array_t argv,
                             const wchar_t* options,
                             const WOption* long_options, int* opt_index) {
    assert(woptind <= argc && "woptind is out of range");
    return wgetopt_inner(argc, argv, options, long_options, opt_index);
}
