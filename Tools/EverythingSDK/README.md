# Everything SDK

Source: https://www.voidtools.com/support/everything/sdk/

Archive: https://www.voidtools.com/Everything-SDK.zip, retrieved 2026-10-21.
Archive SHA-256: `F5716D9513CCE6B462B5170A0A2E7E081E191D9BCAC6774F5DECC061497DF443`.

Only the official C source, public header and IPC header are vendored. Original copyright and MIT permission notices remain in each file. The SDK is compiled into AeluraTool; no SDK DLL or Everything executable is redistributed. The optional backend requires the user's Everything client to already be running with IPC enabled. The default built-in backend remains independent.

Local changes: synchronous query delivery is bounded to two seconds with SendMessageTimeoutW so an unresponsive client cannot indefinitely block a query worker. Core test binaries compile an isolated IPC class override, preventing tests from querying or controlling the user's Everything client. The adapter uses asynchronous queries, a five-second reply deadline and cancellation, and serializes complete SDK transactions across both search surfaces.
