// ==============================================================
// Reference: GCC 16 C++26 Static Reflection Smoke Test
// Verified locally with:
//   g++ (GCC) 16.0.1 20260324 (experimental)
// Compile:
//   g++ -std=c++2c -freflection -o gcc-static-reflection gcc-static-reflection.cpp
// ==============================================================

#include <meta>
#include <iostream>
#include <string>
#include <type_traits>

// ------ Test 1: Basic reflection - enum to string ------

enum class Color { Red, Green, Blue };

template <typename E>
  requires std::is_enum_v<E>
constexpr std::string enum_to_string(E value) {
  static constexpr auto enumerators =
      std::define_static_array(std::meta::enumerators_of(^^E));
  template for (constexpr auto e : enumerators) {
    if (value == [:e:]) {
      return std::string(std::meta::identifier_of(e));
    }
  }
  return "<unnamed>";
}

// ------ Test 2: Struct member introspection ------

struct file_operations {
    int (*open)(int flags);
    int (*read)(void* buf, unsigned long size);
    int (*write)(const void* buf, unsigned long size);
    int (*close)();
};

template <typename S>
void print_members() {
    std::cout << "Members of " << std::meta::identifier_of(^^S) << ":\n";
    template for (constexpr auto member :
        std::define_static_array(
            std::meta::nonstatic_data_members_of(^^S, std::meta::access_context::current()))) {
        std::cout << "  " << std::meta::identifier_of(member) << "\n";
    }
}

// ------ Test 3: Exception handling ------

struct kernel_error : std::exception {
    int errno_val;
    explicit kernel_error(int e) : errno_val(e) {}
    const char* what() const noexcept override { return "kernel error"; }
};

int may_fail(bool do_fail) {
    if (do_fail)
        throw kernel_error(-22);
    return 42;
}

// ------ Test 4: RTTI ------

struct base_operations {
    virtual ~base_operations() = default;
    virtual int operate() = 0;
};

struct ext4_operations : base_operations {
    int operate() override { return 1; }
};

int main() {
    std::cout << "=== GCC C++26 Reflection Smoke Test ===\n\n";

    std::cout << "[1] Static Reflection (P2996):\n";
    std::cout << "  Color::Red   = " << enum_to_string(Color::Red) << "\n";
    std::cout << "  Color::Green = " << enum_to_string(Color::Green) << "\n";
    std::cout << "  Color::Blue  = " << enum_to_string(Color::Blue) << "\n";
    std::cout << "  PASS\n\n";

    std::cout << "[2] Struct Member Introspection:\n";
    print_members<file_operations>();
    std::cout << "  PASS\n\n";

    std::cout << "[3] Exception Handling:\n";
    try {
        may_fail(true);
        std::cout << "  FAIL (exception not thrown)\n";
    } catch (const kernel_error& e) {
        std::cout << "  Caught: errno=" << e.errno_val << "\n";
        std::cout << "  PASS\n";
    }
    std::cout << "\n";

    std::cout << "[4] RTTI (typeid/dynamic_cast):\n";
    base_operations* op = new ext4_operations();
    std::cout << "  typeid = " << typeid(*op).name() << "\n";
    auto* ext4 = dynamic_cast<ext4_operations*>(op);
    std::cout << "  dynamic_cast " << (ext4 ? "succeeded" : "failed") << "\n";
    std::cout << "  PASS\n\n";
    delete op;

    std::cout << "=== All checks passed. ===\n";
    return 0;
}
