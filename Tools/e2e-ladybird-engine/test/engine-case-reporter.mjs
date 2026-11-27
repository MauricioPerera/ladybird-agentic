/*
 * Copyright (c) 2026-present, the Ladybird developers.
 * SPDX-License-Identifier: BSD-2-Clause
 */

export default async function* reportCases(events) {
  for await (const { type, data } of events) {
    if (type === 'test:pass' || type === 'test:fail') {
      const error = data.details?.error?.cause ?? data.details?.error;
      yield JSON.stringify({
        name: data.name,
        passed: type === 'test:pass',
        skipped: Boolean(data.skip),
        todo: Boolean(data.todo),
        ...(error ? { diagnostic: `${error.code}: ${error.message}\n` } : {}),
      }) + '\n';
    } else if (type === 'test:stderr') {
      yield JSON.stringify({ diagnostic: data.message }) + '\n';
    }
  }
}
