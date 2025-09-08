# swift_build_support/products/swift.py -------------------------*- python -*-
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ----------------------------------------------------------------------------

import os
import shutil

from .. import cmake_product
from .. import llvm
from .. import swift
from ... import shell
from ... import cmake

class ExperimentalSwiftStdlib(cmake_product.CMakeProduct):

    @classmethod
    def product_source_name(cls):
        """product_source_name() -> str

        The name of the source code directory of this product.
        """
        return "swift"

    @classmethod
    def is_build_script_impl_product(cls):
        """is_build_script_impl_product -> bool

        Whether this product is produced by build-script-impl.
        """
        return False

    @classmethod
    def is_before_build_script_impl_product(cls):
        return False

    def should_build(self, host_target):
        return self.args.enable_experimental_build_product and self.is_cross_compile_target(host_target)

    def should_test(self, host_target):
        return False

    def should_test_executable(self) -> bool:
        return False

    def should_install(self, host_target):
        return self.should_build(host_target)

    def build(self, host_target):
        llvm_build_dir = self._llvm_build_dir(host_target)
        llvm_cmake_dir = os.path.join(llvm_build_dir, 'lib', 'cmake', 'llvm')

        # Stdlib
        self._build_stdlib(host_target, llvm_cmake_dir)

    def _llvm_build_dir(self, host_target):
        build_root = os.path.dirname(self.build_dir)
        return os.path.join(build_root, 'llvm-%s' % host_target)

    def _build_stdlib(self, host_target, llvm_cmake_dir):
        target_triple = self._get_target_triple(host_target)
        platform, arch = host_target.split('-')
        sysroot = self._get_sysroot(host_target)

        self.cmake_options.define('CMAKE_INSTALL_PREFIX:PATH', '/usr')
        self.cmake_options.define('CMAKE_BUILD_TYPE:STRING', self.args.build_variant)
        self.cmake_options.define(
            'CMAKE_SYSTEM_NAME:STRING', self._get_system_name(platform)
        )
        self.cmake_options.define(
            'CMAKE_SYSTEM_PROCESSOR:STRING', self._get_system_processor(arch)
        )
        self.cmake_options.define('CMAKE_SYSROOT:PATH', sysroot)

        target_c_flags = self._target_c_flags()
        # Add -target and --sysroot since libdispatch sub-dependency of Swift
        # doesn't pick up on CMAKE_SYSTEM_PROCESSOR or CMAKE_SYSROOT. This can be
        # fixed in the future I'm sure.
        target_c_flags += f' -target {target_triple}'
        target_c_flags += f' --sysroot {sysroot}'
        self.cmake_options.define('SWIFT_USE_LINKER:STRING', 'lld')
        self.cmake_options.define('CMAKE_C_FLAGS:STRING', target_c_flags)
        self.cmake_options.define('CMAKE_CXX_FLAGS:STRING', target_c_flags)

        self.cmake_options.define(
            'SWIFT_STDLIB_BUILD_TYPE:STRING', self.args.build_variant
        )

        self.cmake_options.define('CMAKE_C_COMPILER_TARGET:STRING', target_triple)
        self.cmake_options.define('CMAKE_CXX_COMPILER_TARGET:STRING', target_triple)
        self.cmake_options.define('CMAKE_Swift_COMPILER_TARGET:STRING', target_triple)

        if self.args.build_runtime_with_host_compiler:
            self.cmake_options.define(
                'SWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER:BOOL', "TRUE"
            )
            self.cmake_options.define(
                'SWIFT_NATIVE_CLANG_TOOLS_PATH:STRING',
                os.path.dirname(self.toolchain.cc)
            )
            self.cmake_options.define(
                'SWIFT_NATIVE_SWIFT_TOOLS_PATH:STRING',
                os.path.dirname(self.toolchain.swiftc)
            )
            self.cmake_options.define(
                'SWIFT_NATIVE_LLVM_TOOLS_PATH:STRING',
                os.path.dirname(self.toolchain.llvm_ar)
            )
        else:
            # Toolchain configuration
            toolchain_path = self.native_toolchain_path(host_target)
            self.cmake_options.define(
                'SWIFT_NATIVE_CLANG_TOOLS_PATH:STRING',
                os.path.join(toolchain_path, 'bin')
            )
            self.cmake_options.define(
                'SWIFT_NATIVE_SWIFT_TOOLS_PATH:STRING',
                os.path.join(toolchain_path, 'bin')
            )
            self.cmake_options.define(
                'SWIFT_NATIVE_LLVM_TOOLS_PATH:STRING',
                os.path.join(toolchain_path, 'bin')
            )
            self.cmake_options.define(
                'BOOTSTRAPPING_MODE:STRING', 'CROSSCOMPILE')
            self.cmake_options.define(
                'SWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER:BOOL', 'FALSE')

        self.cmake_options.define('CMAKE_C_COMPILER_WORKS:BOOL', 'TRUE')
        self.cmake_options.define('CMAKE_CXX_COMPILER_WORKS:BOOL', 'TRUE')
        self.cmake_options.define('CMAKE_Swift_COMPILER_WORKS:BOOL', 'TRUE')
        self.cmake_options.define('LLVM_COMPILER_CHECKED:BOOL', 'TRUE')

        self.cmake_options.define('LLVM_DIR:PATH', llvm_cmake_dir)

        # Standalone stdlib configuration
        self.cmake_options.define('SWIFT_INCLUDE_TOOLS:BOOL', 'FALSE')
        self.cmake_options.define('SWIFT_INCLUDE_DOCS:BOOL', 'FALSE')
        self.cmake_options.define('SWIFT_BUILD_REMOTE_MIRROR:BOOL', 'TRUE')
        self.cmake_options.define('SWIFT_BUILD_SOURCEKIT:BOOL', 'FALSE')

        # Stdlib configuration
        self.cmake_options.define('SWIFT_SDKS:STRING', platform.upper())
        self.cmake_options.define(
            'SWIFT_SDK_%s_ARCH_%s_PATH' % (platform.upper(), arch.lower()),
            sysroot
        )

        # Disable overlay that's hardcoded to point to /usr
        if platform == 'linux':
            self.cmake_options.define(
                'SWIFT_SDK_LINUX_CXX_OVERLAY_SWIFT_COMPILE_FLAGS:STRING', ''
            )

        # Library features
        self.cmake_options.define('SWIFT_BUILD_DYNAMIC_STDLIB:BOOL', 'TRUE')
        self.cmake_options.define(
            'SWIFT_BUILD_STATIC_STDLIB:BOOL',
            'TRUE' if self.args.build_swift_static_stdlib else 'FALSE'
        )
        self.cmake_options.define('SWIFT_BUILD_STATIC_SDK_OVERLAY:BOOL', 'TRUE')
        self.cmake_options.define(
            'SWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY:BOOL', 'TRUE'
        )
        self.cmake_options.define(
            'SWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED:BOOL', 'TRUE'
        )
        self.cmake_options.define('SWIFT_ENABLE_SYNCHRONIZATION:BOOL', 'TRUE')
        self.cmake_options.define('SWIFT_ENABLE_VOLATILE:BOOL', 'TRUE')
        self.cmake_options.define(
            'SWIFT_ENABLE_EXPERIMENTAL_OBSERVATION:BOOL', 'TRUE'
        )
        self.cmake_options.define(
            'SWIFT_SHOULD_BUILD_EMBEDDED_STDLIB:BOOL', 'FALSE'
        )

        # Source directories
        source_dir = os.path.dirname(self.source_dir)
        self.cmake_options.define(
            'SWIFT_PATH_TO_LIBDISPATCH_SOURCE:PATH',
            os.path.join(source_dir, 'swift-corelibs-libdispatch')
        )
        self.cmake_options.define(
            'SWIFT_ENABLE_EXPERIMENTAL_STRING_PROCESSING:BOOL', 'TRUE'
        )
        self.cmake_options.define(
            'SWIFT_PATH_TO_STRING_PROCESSING_SOURCE:PATH',
            os.path.join(source_dir, 'swift-experimental-string-processing')
        )

        # NOTE: Tests are currently disabled due to cross-compilation not being
        # fully supported yet. This will be revisited in the future.
        self.cmake_options.define('SWIFT_INCLUDE_TESTS:BOOL', 'FALSE')

        # armv7 currently requires c11 threading model due to pthreads issues
        if arch == 'armv7':
            self.cmake_options.define('SWIFT_THREADING_PACKAGE:STRING', 'c11')

        self.build_with_cmake(
            [], self.args.build_variant, [],
            prefer_native_toolchain=not self.args.build_runtime_with_host_compiler
        )

        # Copy swiftrt.o to the sysroot. This is required since -sdk looks for
        # swiftrt.o at <sysroot>/usr/lib/swift/linux/<arch> instead of the
        # -resource-dir, which may be a bug?
        arch_dir = os.path.join(sysroot, 'usr', 'lib', 'swift', platform, arch)
        shell.makedirs(arch_dir)
        shutil.copy(
            os.path.join(self.build_dir, 'lib', 'swift', platform, arch, 'swiftrt.o'),
            arch_dir
        )

    def install(self, host_target):
        """
        Perform the install phase for the product.

        This phase might copy the artifacts from the previous phases into a
        destination directory.
        """
        host_install_destdir = self.host_install_destdir(host_target)
        install_targets = ['install']

        self.install_with_cmake(install_targets, host_install_destdir)

    @classmethod
    def get_dependencies(cls):
        return [llvm.LLVM,
                swift.Swift]

    def _get_cross_compile_target_index(self, host_target):
        return self.args.cross_compile_hosts.index(host_target)

    def _get_sysroot(self, host_target):
        if not self.args.cross_compile_sysroots:
            raise ValueError("Sysroot must be specified for each crosscompilation"
                             "  target using --cross-compile-sysroots")
        return self.args.cross_compile_sysroots[self._get_cross_compile_target_index(host_target)]

    def _target_c_flags(self):
        # lld is required to link for cross-compilation
        flags = '-w -fuse-ld=lld'

        if self.args.cross_compile_flags:
            index = self._get_cross_compile_target_index(host_target)
            flags += f' {self.args.cross_compile_flags[index]}'

        return flags

    def _get_system_name(self, platform):
        if platform == 'linux':
            return 'Linux'
        elif platform == 'freebsd':
            return 'FreeBSD'
        elif platform == 'openbsd':
            return 'OpenBSD'
        else:
            raise ValueError(f"Unsupported platform for cross-compilation: {platform}")

    def _get_system_processor(self, arch):
        if arch == 'armv7':
            return 'armv7-a'
        return arch

    def _get_target_triple(self, host_target):
        (platform, arch) = host_target.split('-')
        if platform == 'linux':
            if arch in ('armv6', 'armv7'):
                return f"{arch}-unknown-{platform}-gnueabihf"
            else:
                return f"{arch}-unknown-{platform}-gnu"
        elif platform == 'freebsd' or platform == 'openbsd':
            return f"{arch}-unknown-{platform}"
        else:
            raise ValueError(f"Unsupported platform for cross-compilation: {platform}")
