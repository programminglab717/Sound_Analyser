# Architecture Decision Records

One record per decision that is expensive to reverse. Format: context, decision,
consequences, and what would make us revisit.

| ADR | Decision | Status |
| --- | --- | --- |
| [0001](0001-cpp20-juce.md) | C++20 + JUCE as the application stack | ⚠️ Superseded by 0006 |
| [0002](0002-closed-source-juce-starter.md) | Closed-source under the JUCE Starter tier | ⚠️ Superseded by 0006 |
| [0003](0003-non-destructive-document-model.md) | Non-destructive EDL document model | Accepted |
| [0004](0004-on-device-ml-downloaded-models.md) | On-device ML, models downloaded not bundled | Accepted |
| [0005](0005-out-of-process-plugin-hosting.md) | Out-of-process plugin hosting | Accepted |
| [**0006**](0006-permissive-only-dependencies.md) | **Permissive-only dependencies; drop JUCE** | **Accepted** |
| [**0007**](0007-spectrogram-rendering.md) | **Spectrogram as a level pyramid uploaded to a shader** | **Accepted** |

Superseded records are kept rather than deleted — the reasoning that was
reversed is part of the history, and 0006 is only legible against 0001/0002.
