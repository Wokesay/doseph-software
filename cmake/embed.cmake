# SPDX-License-Identifier: AGPL-3.0-or-later
# Writes embedded.cpp with the version and the recipe templates. The texts
# are byte arrays: MSVC limits string literals to about 16 KB.
function(dp_embed_bytes var file out)
  file(READ "${file}" hex HEX)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  # wrap lines (CMake's regex has no {n}): 24 bytes per line
  string(REPEAT "0x[0-9a-f][0-9a-f]," 24 row)
  string(REGEX REPLACE "(${row})" "\\1\n  " bytes "${bytes}")
  file(APPEND "${out}" "static const unsigned char ${var}Data[] = {\n  ${bytes}0x00};\n")
  file(APPEND "${out}" "const char* const ${var} = reinterpret_cast<const char*>(${var}Data);\n")
endfunction()

file(WRITE "${OUT}" "// Written by cmake/embed.cmake. Do not edit by hand.\n")
file(APPEND "${OUT}" "#include \"doseph/embedded.hpp\"\nnamespace doseph::embedded {\n")
file(APPEND "${OUT}" "const char* const kVersion = \"${VERSION}\";\n")
dp_embed_bytes(kTemplatesJson "${TEMPLATES}" "${OUT}")
file(APPEND "${OUT}" "}  // namespace doseph::embedded\n")
