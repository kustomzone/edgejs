<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./assets/edgejs-logo-white.svg">
    <source media="(prefers-color-scheme: light)" srcset="./assets/edgejs-logo-dark.svg">
    <img src="./assets/edgejs-logo-dark.svg" alt="Edge.js logo" height="120">
  </picture>
</p>

<p align="center">
  Run JavaScript anywhere. <b>Safely</b>.
</p>

<hr />

Edge.js is a **Node.js-compatible** runtime with stronger sandboxing than any other JS runtime thanks to WebAssembly.

✅ **Full compatibility with Node**: use your codebase, packages, and workflow.
🛡️ **Sandboxed by design**: built for serverless and embedded workloads.
🧩 **Choose the JS engine**: V8, JavaScriptCore or QuickJS.
💪 **Compatible with NPM/PNPM/Yarn/Bun**: use your current package manager with `ubi`.

## Install Edge.js

```bash
curl -fsSL https://edgejs.org/install | bash
```

## Use it!

You can use it as you would do it with Node.js:

```js
const http = require("node:http");

http
  .createServer((_req, res) => {
    res.end("hello from edge\n");
  })
  .listen(3000, () => {
    console.log("listening on http://localhost:3000");
  });
```

```bash
$ ubi server.js
```

If you want to use it in your current workflow, just wrap your commands with `ubi`:

```bash
$ ubi node myfile.js
$ ubi npm install
$ ubi pnpm run dev
```

## Development

Build the CLI locally:

```bash
make build
./build-ubi/ubi server.js
```

```bash
./build-ubi/ubi --run dev
```

Or run the tests:
```bash
make test
NODE_TEST_RUNNER="$(pwd)/build-ubi/ubi" \
./node-test/nodejs_test_harness --category=node:assert
```


## Roadmap

### [Contribute to our ROADMAP](https://github.com/wasmerio/ubi/issues/8)

- `0.x` Production readiness: platform coverage across Linux, Windows, macOS, iOS, and Android; reliability in constrained environments; security audits; and successful real production use.
- `1.x` Need for speed: faster startup, faster core paths, and performance that competes with or beats Node.js, Bun, and Deno on most workloads.
- `2.x` Enhancements: first-class TypeScript support and a smoother developer experience.

For architecture detail, see [`ubi/README.md`](./ubi/README.md) and [`ubi/ROADMAP.md`](./ubi/ROADMAP.md).
