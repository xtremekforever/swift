// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -enable-experimental-feature Extern -enable-experimental-feature Embedded -enforce-exclusivity=none %s -c -o %t/a.o
// RUN: %target-clang %t/a.o -o %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: executable_test
// REQUIRES: OS=macosx || OS=linux-gnu

@_silgen_name("putchar")
@discardableResult
func putchar(_: CInt) -> CInt

public func print(_ s: StaticString, terminator: StaticString = "\n") {
  var p = s.utf8Start
  while p.pointee != 0 {
    putchar(CInt(p.pointee))
    p += 1
  }
  p = terminator.utf8Start
  while p.pointee != 0 {
    putchar(CInt(p.pointee))
    p += 1
  }
}

print("Hello, Embedded Swift!")
// CHECK: Hello, Embedded Swift!
