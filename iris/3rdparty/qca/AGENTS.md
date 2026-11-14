# Repository instructions

QCA is a general-purpose cryptography library. Providers implement reusable
algorithms, protocols and QCA API contracts for any compatible caller.

## Documentation and language

- All documentation committed or pushed to this repository must be in English.
  This includes READMEs, audit reports, design notes, changelogs, contribution
  instructions, and prose added to code comments or test descriptions.
- Deliberately non-English test data and localization resources are allowed;
  describe their purpose in English.
- Keep published documentation, code comments and tests independent of specific
  downstream applications or frameworks. Describe requirements through public
  APIs, algorithms, formats and protocol standards.
- Do not add downstream project names, application-specific build flags, local
  workspace paths, integration reports or deployment recipes to repository
  content. Keep such working notes outside version control.
- Preserve required copyright and license attribution and the library's actual
  upstream, source and support URLs.

## Implementation and verification

- Do not introduce application-specific behavior or special cases. Add a
  reusable capability or fix the relevant library/provider contract instead.
- Document partial provider support explicitly. A key type or protocol feature
  name must not be presented as proof that all related operations work.
- Verify public API behavior, unsupported operations, failure paths, secret
  lifetimes and interoperability as appropriate for a cryptographic change.
- Keep cryptographic processing bounded and linear where applicable. Avoid
  repeated whole-message copies and unnecessary retention of sensitive data.
- Do not expand a documentation cleanup into new cryptographic functionality.
