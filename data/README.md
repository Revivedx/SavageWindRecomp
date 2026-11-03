# amiibo catalog attribution

The offline metadata in `amiibo.json` and the generated `runtime/amiibo_catalog.h`
is adapted from [AmiiboAPI](https://amiiboapi.org/), maintained by
[8bitDream and contributors](https://github.com/8bitDream/AmiiboAPI), building on
the original [AmiiboAPI by Nevin Vu (N3evin) and contributors](https://github.com/N3evin/AmiiboAPI).

Upstream's MIT license and copyright notice are preserved in
[`AmiiboAPI-LICENSE.txt`](AmiiboAPI-LICENSE.txt). Release packages include this notice.

Savage Wind adapts the API's names, IDs, series, characters and types into an
offline catalog. Some names and groups are normalized, and entry order and saved
keys are kept stable by `tools/update_amiibo_catalog.py`. No upstream images,
figure dumps or keys are bundled, and the game does not contact the API.

AmiiboAPI is not affiliated with Nintendo. This attribution does not grant rights
to Nintendo's or other owners' characters, trademarks or game assets.
