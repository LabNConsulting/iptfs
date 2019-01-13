#!/usr/bin/env python
# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 13 2019, Christian E. Hopps <chopps@gmail.com>
#
from setuptools import setup, Extension

bstr = Extension('pyisis.bstr', sources=['tcptfs/bstr.c'])

setup(
    setup_requires=['pbr'],
    pbr=True,
    ext_modules=[bstr]
)
