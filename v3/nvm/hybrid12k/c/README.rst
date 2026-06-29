A1 C Encoder/Decoder
====================

This directory contains the final A1 implementation:

- ``hy_enc``: host-side encoder. Compression-side CPU and memory are intentionally
  unconstrained; built from ``patch_generate/``.
- ``hy_dec``: host demo harness for the reusable streaming in-place decoder,
  including a SAML22-shaped NVM emulator.
- ``patch_apply/patch_apply.h``: production decoder artifact for the device
  build. Include this header from one application translation unit and provide
  ``flash_read()``, ``flash_write()``, and ``g_image_span``.

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

   arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -Os -DRC_V3_ARM -I . -I common -x c -c patch_apply/patch_apply.h -o /tmp/patch_apply_arm.o
   arm-none-eabi-size /tmp/patch_apply_arm.o

The encoder ``W`` argument must match decoder ``SA_W``. The production default is
``W=10`` / ``SA_W=10``.
