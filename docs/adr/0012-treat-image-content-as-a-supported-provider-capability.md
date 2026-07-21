---
status: accepted
---

# Treat image content as a supported provider capability

Image content remains part of the public Provider Message contract and therefore receives end-to-end preservation through supported provider adapters. An adapter may explicitly reject an image when its provider or model lacks image input, but it may not silently replace image bytes with text; product policy such as blocking images belongs at the coding-agent conversion boundary and must be explicitly enabled.

## Considered options

- Keep the public image alternative but replace it with a placeholder: rejected because it silently loses caller data while pretending the call is supported.
- Remove image content and defer multimodal support: rejected because the existing provider contract and pi interoperability make end-to-end support more valuable than shrinking the surface.
- Preserve images and reject unsupported destinations explicitly: accepted because it gives callers deterministic capability semantics.

## Consequences

- MIME type and encoded image data reach supporting provider requests unchanged.
- Provider/model incompatibility produces an explicit capability error through the provider terminal contract.
- Image blocking or replacement occurs only under explicit coding-agent policy, never as an AI-adapter default.
- Request fixtures and round-trip tests verify that image content is not lost.
