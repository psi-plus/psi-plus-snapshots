# Installed shared SDK regression

This project uses only `find_package(Iris CONFIG)` and `Iris::Iris`. Copy this
directory outside the Iris checkout, install shared Iris into a private prefix,
remove the producer source/build trees, then run:

```sh
cmake -S sdk-consumer -B sdk-consumer-build -DCMAKE_BUILD_TYPE=Release \
    -DIris_DIR=/tmp/iris-sdk/lib/cmake/Iris
cmake --build sdk-consumer-build
ctest --test-dir sdk-consumer-build --output-on-failure
python3 sdk-consumer/check-exports.py /tmp/iris-sdk/lib/libiris-qt6.so
```

The consumer references public irisnet methods, QObject metadata and RTTI,
including API not used directly by the XMPP layer. It does not start networking
or install process signal handlers. Volatile stores keep link references alive
under optimization. The symbol-table check is Linux-specific; the consumer
project works with either supported Qt major version.

The original shared build contained all irisnet archive members, including the
four symbols needed by Psi, but made them local: hidden visibility combined
with unconditional `IRISNET_STATIC` suppressed existing export annotations,
and several public classes lacked annotations. Keep archive linkage and hidden
implementation visibility; export the installed public API explicitly instead.
`HttpProxyPost` and `HttpProxyGetStream` are internal HttpPoll helpers and are
deliberately excluded, along with pimpls and non-installed implementation types.
