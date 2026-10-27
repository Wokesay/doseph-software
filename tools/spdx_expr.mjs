// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX license expressions for tools/check_licenses.mjs (tested in
// tools/check_licenses.test.mjs).

/**
 * SPDX expression with AND, OR and brackets: OR needs one allowed side, AND
 * both. Anything else (WITH, lower-case operators, missing operators, stray
 * brackets) makes the expression invalid, and an invalid one is never
 * allowed.
 */
export function allowed(expr, ok) {
  const tokens = String(expr).replace(/[()]/g, " $& ").split(/\s+/).filter(Boolean);
  const OPS = new Set(["AND", "OR", "WITH", "(", ")"]);
  let i = 0;
  let valid = tokens.length > 0;
  const atom = () => {
    const t = tokens[i++];
    if (t === "(") {
      const v = or();
      if (tokens[i++] !== ")") valid = false;
      return v;
    }
    if (t === undefined || OPS.has(t) || !/^[A-Za-z0-9.+-]+$/.test(t) || /^(and|or|with)$/i.test(t)) {
      valid = false;
      return false;
    }
    return ok.has(t);
  };
  const and = () => {
    let v = atom();
    while (tokens[i] === "AND") {
      i++;
      v = atom() && v;
    }
    return v;
  };
  const or = () => {
    let v = and();
    while (tokens[i] === "OR") {
      i++;
      v = and() || v;
    }
    return v;
  };
  const v = or();
  return valid && i === tokens.length && v;
}
