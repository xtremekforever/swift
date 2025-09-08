# swift_build_support/products/experimental/foundation.py ------*- python -*-
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2025 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ----------------------------------------------------------------------------

import os
import shutil

from .. import cmark
from .. import libcxx
from .. import libdispatch
from .. import cmake_product
from .. import llvm
from .. import swift
from ... import cmake

class ExperimentalLibdispatch(cmake_product.CMakeProduct):
    @classmethod
    def is_build_script_impl_product(cls):
        """is_build_script_impl_product -> bool

        Whether this product is produced by build-script-impl.
        """
        return False

    @classmethod
    def is_before_build_script_impl_product(cls):
        """is_before_build_script_impl_product -> bool

        Whether this product is built before any build-script-impl products.
        """
        return False

    @classmethod
    def product_source_name(cls):
        """product_source_name() -> str

        The name of the source code directory of this product.
        """
        return "swift-corelibs-libdispatch"

    def should_build(self, host_target):
        return self.args.enable_experimental_build_product and self.is_cross_compile_target(host_target)

    def should_test(self, host_target):
        return False

    def should_test_executable(self) -> bool:
        return False

    def should_install(self, host_target):
        return self.should_build(host_target)

    def _swift_build_dir(self, host_target):
        build_root = os.path.dirname(self.build_dir)
        return os.path.join(build_root, 'experimentalswiftstdlib-%s' % host_target)

    def append_platform_cmake_options(self, host_target):
        (platform, arch) = host_target.split('-')
        swift_build_dir = self._swift_build_dir(host_target)
        swift_resource_dir = os.path.join(swift_build_dir, 'lib', 'swift')
        swift_flags = ['-use-ld=lld',
                       '-sdk', self.get_linux_sysroot(platform, arch),
                       '-resource-dir', swift_resource_dir]

        self.cmake_options.define('CMAKE_Swift_COMPILER_TARGET', self.get_linux_target(platform, arch))
        self.cmake_options.define('CMAKE_Swift_FLAGS', ' '.join(swift_flags))
        self.cmake_options.define('CMAKE_BUILD_TYPE:STRING', self.args.build_variant)

    def build(self, host_target):
        self._build(host_target)
        if self.args.build_swift_static_stdlib:
            self._build(host_target, static=True)

    def _build(self, host_target, static=False):
        if static:
            self.build_dir = os.path.join(
                os.path.dirname(self.build_dir),
                'experimentallibdispatch-static-%s' % host_target
            )

        self.cmake_options.define(
            'BUILD_SHARED_LIBS:BOOL', 'FALSE' if static else 'TRUE'
        )
        self.cmake_options.define('ENABLE_SWIFT:BOOL', 'TRUE')

        self.append_platform_cmake_options(host_target)
        self.generate_toolchain_file_for_darwin_or_linux(host_target)

        self.build_with_cmake(
            [], self.args.build_variant, [],
            prefer_native_toolchain=not self.args.build_runtime_with_host_compiler,
            ignore_extra_cmake_options=True
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
        return [cmark.CMark,
                llvm.LLVM,
                libcxx.LibCXX,
                swift.Swift]

    @classmethod
    def is_nondarwin_only_build_product(cls):
        return True
