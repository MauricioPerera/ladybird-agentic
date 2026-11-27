---
type: 'Task Contract'
title: 'Pinned e2e oracle for the Ladybird adapter'
description: 'Measure both existing Engine Protocol cases with the pinned public e2e package.'
tags: ['testing', 'node', 'e2e', 'kdd']
task: ladybird-e2e-adapter-oracle
intent: 'Make the existing Engine Protocol integration test mandatory and reproducible.'
target: Tools/e2e-ladybird-engine/test/engine-protocol.test.mjs
language: javascript
signature: 'node --test Tools/e2e-ladybird-engine/test/engine-protocol.test.mjs'
test_command: 'node --test Tools/e2e-ladybird-engine/test/pinned-engine-oracle.test.mjs'
test_runner: node-test
test_timeout: 60
tests: Tools/e2e-ladybird-engine/test/pinned-engine-oracle.test.mjs
tests_sha256: 'a8634f6cd9c9e90d0bd68eb0ec8ec5d62360cc753b3a1964b095db3b0d3eb530'
oracle_files: ['Tools/e2e-ladybird-engine/test/pinned-engine-oracle.test.mjs', 'Tools/e2e-ladybird-engine/test/engine-case-reporter.mjs', 'Tools/e2e-ladybird-engine/package.json', 'Tools/e2e-ladybird-engine/package-lock.json', 'Tools/e2e-ladybird-engine/index.mjs', 'Tools/e2e-ladybird-engine/server.mjs']
touch_only: ['Tools/e2e-ladybird-engine/test/engine-protocol.test.mjs', 'Tools/e2e-ladybird-engine/README.md']
budget:
  cyclomatic_max: 8
  nesting_max: 3
deps_allowed: ['node', 'e2e@0.24.0']
forbids: ['llm']
---

## Intent

Run the Ladybird adapter against a loopback MCP fixture with the same Node
and e2e versions as the KDD-Board browser pilot. The [adapter](../../Tools/e2e-ladybird-engine/README.md)
remains independently usable by a consumer with a live browser.

## Interface

Install the tool-local dependencies with `npm ci --prefix Tools/e2e-ladybird-engine --ignore-scripts`.
From the repository root, `node --test Tools/e2e-ladybird-engine/test/engine-protocol.test.mjs`
runs both existing Engine Protocol cases. The `connectEngine` and
`spawnTransport` integration imports the public `e2e/engine` entry point.

## Invariants

- Use Node 24.16.0 and the locked `e2e` 0.24.0 distribution for the verification run.
- Preserve both existing case names and assertions. A run with the dependencies
  installed executes both cases successfully and records zero skips or todos.
- An absent `e2e` dependency makes the run fail. The tests use the installed
  package even if `E2E_REPO_PATH` names an absent source checkout.
- Only the synthetic MCP fixture is contacted. The native adapter, protocol
  implementation, browser build and existing CI keep their current behavior.
- Record the two Windows and Ubuntu passes against the candidate, including
  the IDs reported by the KDD runner. The loopback fixture verifies the
  protocol boundary. Native compilation, DOM behavior and C++ coverage
  require separate evidence.

## Examples

- Locked install and an absent `E2E_REPO_PATH` target yield two passing cases.
- A temporary checkout containing the adapter without `node_modules` fails
  explicitly on loading `e2e/engine`.

## Do / Don't

Use the package lock for dependency preparation and the public export for the
real client. Treat omitted tests as a rejection.

## Tests

Run the `test_command` with the locked dependencies. The oracle starts the
existing suite in child Node processes and collects native test events to
verify exact case names and statuses. It also runs an isolated copy without
dependencies and checks for a missing-package failure. Freeze the oracle,
reporter, dependencies and adapter blobs in the approved reference.

## Constraints

Keep production sources and workflows outside the implementation perimeter.
PARAR y reportar si el cambio requiere modificar el protocolo o el CI nativo.
Use the [KDD quality protocol](https://github.com/MauricioPerera/KDD/blob/e318f544bc0f3cc17d8a02ded5f07b2883a385b5/knowledge/quality-approval.md)
for review of this contract and oracle before implementing the change.
