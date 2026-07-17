# Third-party notices — CircuitPython test corpus

The committed binaries under `test-bench/foreign/` are derived from official
Adafruit CircuitPython releases for the `feather_m0_express` board. They are
test data, not UltraPatch-authored firmware.

The raw releases are `2.2.0`, `2.2.1`, `2.2.2`, `2.2.3`, `2.2.4`,
`2.3.0`, `2.3.1`, `3.0.0`, `3.0.1`, `3.0.2`, and `3.0.3`. The UF2
releases are `10.0.0`, `10.0.1`, `10.0.2`, `10.0.3`, `10.1.1`,
`10.1.2`, and `10.1.3`; they were unpacked at application base `0x2000` to
match the raw image layout. The committed files and their Git history are the
frozen corpus provenance record.

Official artifacts came from the Adafruit CircuitPython listings for
[older raw binaries](https://adafruit-circuit-python.s3.amazonaws.com/index.html?prefix=bin/feather_m0_express/en_US/OLD/)
and [current UF2 releases](https://adafruit-circuit-python.s3.amazonaws.com/index.html?prefix=bin/feather_m0_express/en_US/).
Corresponding source and per-file notices are available from the release tags
and history in the
[Adafruit CircuitPython repository](https://github.com/adafruit/circuitpython).
The available 2.x/3.x release tags carry this root notice:

> Copyright (c) 2013, 2014 Damien P. George

The available 10.x release tags carry this updated root notice:

> Copyright (c) 2013-2025 Damien P. George

Both grant the following MIT license:

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to deal
> in the Software without restriction, including without limitation the rights
> to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
> copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
> OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
> THE SOFTWARE.

Individual CircuitPython source files and bundled components may name
additional copyright holders or licenses; the corresponding source headers and
license files in the upstream release history are authoritative for those
components.
