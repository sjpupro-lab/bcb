# BCB — Binary Compression by BT
# Copyright (c) 2026 호시 <jahyag@gmail.com>
# Proprietary — All Rights Reserved. See LICENSE.
#
# Minimal shim: project metadata lives in pyproject.toml. setuptools needs the
# cffi_modules keyword here so the _bcb_cffi extension is built.
from setuptools import setup

setup(cffi_modules=["bcb_build.py:ffibuilder"])
