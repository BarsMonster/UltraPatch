A1 C Encoder/Decoder
====================

This directory contains the final A1 implementation:

- ``hy_enc``: host-side encoder. Compression-side CPU and memory are intentionally
  unconstrained.
- ``hy_dec``: host harness for the production streaming in-place decoder, using
  the SAML22-shaped NVM emulator in ``flash_nvm.c``.
- ``rc_v3.c``: production decoder source for the device build.

Build
-----

.. code-block:: sh

   make

Encode and decode the real one-face update:

.. code-block:: sh

   ./hy_enc ../fixtures/v0_base ../fixtures/v1_one_face /tmp/grow.blob 10
   cp ../fixtures/v0_base/watch.bin /tmp/mem.bin
   ./hy_dec /tmp/mem.bin /tmp/grow.blob 1
   cmp /tmp/mem.bin ../fixtures/v1_one_face/watch.bin

Or run the built-in C-only smoke check:

.. code-block:: sh

   make check

Device Object Check
-------------------

.. code-block:: sh

   arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -Os -DRC_V3_ARM -I . -c rc_v3.c -o /tmp/rc_v3_arm.o
   arm-none-eabi-size /tmp/rc_v3_arm.o

The encoder ``W`` argument must match decoder ``SA_W``. The production default is
``W=10`` / ``SA_W=10``.
