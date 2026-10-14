# Burnos — the reveal film

Original editable motion graphics for the **2:59 Burnos product reveal**. The main composition is `Burnos-Official-Film`; individual chapters are also available in the studio.

```sh
npm ci
npm run dev
```

Render the film:

```sh
npx remotion render Burnos-Official-Film out/burnos.mp4 --codec=h264 --crf=18 --pixel-format=yuv420p
```

Render a still:

```sh
npx remotion still 01-Reveal out/hero.png --frame=300
```

The source, timed captions, and compressed mastered soundtrack are included. The committed MP4 was rendered with the original lossless soundtrack; rerenders use the AAC master supplied here. No local speech model or API key is needed to render. Segoe UI/Arial are requested as system fonts, so appearance can vary across operating systems. Font files are not redistributed.

## What the film shows

The desktop panels and workflows are **illustrated product simulations**, not native UI captures. Aurora is a fictional workshop. The persistence chapter summarizes a real, isolated Mnemosyne test; the [evidence and its limits](../../docs/hackathon/DEMO.md) are provided separately.

Original music was synthesized for this film. Narration uses a local Kokoro synthetic voice. No third-party song recording or celebrity voice imitation was used. Original project code and media are covered by the root MIT license; installed dependencies, including Remotion, retain their own terms. See [third-party notices](../../THIRD_PARTY_NOTICES.md).
