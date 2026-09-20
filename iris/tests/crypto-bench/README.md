# QCA3 / OpenSSL short benchmark

Builds against the **installed** QCA3, explicitly selecting `qca-ossl`, and OpenSSL 3.
It does not build or modify QCA, Iris or Psi. AES-128-CTR and HMAC-SHA1 are primitives
relevant to one SRTP profile, not a full SRTP implementation or a DTLS benchmark.

```sh
cmake -S iris/tests/crypto-bench -B /tmp/iris-crypto-bench -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/iris-crypto-bench -j2
taskset -c 0 /tmp/iris-crypto-bench/qca_crypto_bench 0.2 > /tmp/normal.csv
taskset -c 0 /tmp/iris-crypto-bench/qca_crypto_bench 0.2 reverse > /tmp/reverse.csv
```

Run sequentially. Each run takes approximately 4.8 seconds. Choose an allowed CPU
on your machine, or omit taskset. The second run reverses the backend order within
each pair; it does not eliminate frequency scaling or thermal/background noise.

Each case warms up, then measures batches of 64 operations. CSV contains operation
counts, wall time, process CPU time, ns/operation and payload MiB/s. Ciphertext and
MAC outputs are checked for equality before timing. Fixed keys/IVs and input are
synthetic: **never reuse these IVs for production encryption**.

`reuse` reuses a context but reinitializes it with the key for every message on
both paths. `fresh` creates/destroys a context per message. OpenSSL algorithm
handles are cached; QCA fresh-object lookup costs are included. This compares
practical API paths, not just virtual-call overhead or identical internal calls.
Input is preallocated QByteArray-backed MemoryRegion. QCA output allocation and
any conversion to secure buffers are timed; OpenSSL writes into a reusable caller
buffer. No explicit QCA output-to-QByteArray copy is added. Different buffer and
key-reuse APIs could change these results.

## Sample results

`sample-normal.csv` and `sample-reverse.csv`: Intel Core i7-1255U, logical CPU 0,
Release/GCC 15.2.0, QCA3 3.0.3, Qt 6.10.2, OpenSSL 3.5.5. The installed QCA3
qca-ossl plugin and benchmark resolve the same system libcrypto.so.3.
These are two short observations, not confidence intervals.

CPU time per message, context reused (microseconds; range across both runs):

| Primitive | Bytes | OpenSSL | QCA |
|---|---:|---:|---:|
| AES-128-CTR | 160 | 0.48–0.50 | 1.11–1.26 |
| HMAC-SHA1 | 160 | 0.61–0.62 | 1.20–1.22 |
| AES-128-CTR | 1200 | 0.57–0.91 | 1.45–1.76 |
| HMAC-SHA1 | 1200 | 1.17–1.27 | 1.64–2.00 |
| AES-128-CTR | 16384 | 2.37–2.77 | 14.54–17.39 |
| HMAC-SHA1 | 16384 | 9.02–10.03 | 9.35–9.47 |

Fresh QCA contexts at 1200 bytes cost 4.02–4.83 us for AES and 5.18–5.93 us
for HMAC, compared with 1.45–1.76 and 1.64–2.00 us when reused.

The large AES gap deserves allocation/copy profiling, including a preallocated
SecureArray input variant. Source inspection shows the cipher provider accepts
SecureArray input and resizes output around EVP_EncryptUpdate. This is a hypothesis
for the cost, not a measured attribution of the gap to those operations.

No conclusions about DTLS/SCTP throughput, AES-GCM, decryption, Android battery
usage or temperature follow from these numbers. In particular QCA TLS delegates
record cryptography to its TLS backend rather than necessarily processing records
through this public Cipher API. Measure that path separately before changing it.

## DTLS record benchmark

`qca_dtls_bench` compares established QCA TLS Datagram and native OpenSSL sessions.
It requires OpenSSL 3.2+ for datagram-preserving memory BIOs. No UDP sockets or
external accounts are used. One process performs encryption on the sender,
in-memory datagram relay and decryption on the receiver, verifying every payload.
Certificate generation, pinned-peer checking and handshakes are outside timing.

```sh
taskset -c 0 /tmp/iris-crypto-bench/qca_dtls_bench 0.5 > /tmp/dtls-normal.csv
taskset -c 0 /tmp/iris-crypto-bench/qca_dtls_bench 0.5 reverse > /tmp/dtls-reverse.csv
```

Each run measures six cases for about three seconds total, plus setup. Both
connections use DTLS 1.2, ECDHE-RSA-AES256-GCM-SHA384, MTU 1400. The actual
negotiated cipher is checked. The installed QCA selected AES-256 even when
AES-128 was requested via setConstraints (tried both OpenSSL and IANA names);
that unexpected constraint behavior needs separate investigation. No mismatched
cipher runs are included in the samples.

CPU microseconds per complete sender/receiver record cycle on the same machine:

| Plaintext bytes | Native OpenSSL | QCA |
|---:|---:|---:|
| 160 | 1.18–1.24 | 5.18–5.74 |
| 512 | 1.31–2.19 | 5.13–5.37 |
| 1200 | 1.56–2.53 | 5.30–5.38 |

QCA handles approximately 212–216 payload MiB/s at 1200 bytes in this synthetic
two-endpoint loop, versus 451–730 MiB/s for native OpenSSL. Short-run variability
is substantial for the native path; these ranges are observations, not statistical
confidence bounds. Raw samples are in `dtls-normal.csv` and `dtls-reverse.csv`.

This is a practical API-path comparison, **not isolated AES cost**: QCA includes
its event processing, signals, queueing and QByteArray buffers; native OpenSSL
uses synchronous SSL calls, memory BIOs and reusable stack buffers. Both process
the same plaintext, but the surrounding implementations are deliberately different.
The QCA implementation here is not the additional Iris Dtls wrapper.

At 100 Mbit/s of 1200-byte plaintext records, linear extrapolation gives about
5.5–5.6% of one CPU for QCA and 1.6–2.6% for native OpenSSL, **summing both ends**.
This is not the load of either individual endpoint, nor a measured file-transfer
result. SCTP, ICE, loss/retransmission, socket I/O, packet batching, Android energy
consumption and thermal behavior remain outside this benchmark.
