# Your name on the map (optional)

Off by default. By default your station is anonymous: the map shows what was heard, not who heard it.

In OnAir, Server > Your name on the map lets you type a name. Nothing is sent until you type it and press "Use this name"; OnAir never takes it from your computer's user name. "Stay anonymous" removes it again.

What the name does, in plain words:
- It is public. Anyone can see it.
- It is shown on every icon you were the last to hear ("Last heard by NAME 12 s ago"). If another station hears the same aircraft or ship after you, their name (or none) replaces yours.
- It can be searched on map.on-air.dev: the search box lists matching users and the number of icons; picking one shows only that user's icons.
- It is sent with each upload as an optional `by` field next to `station`. Old servers ignore it; old apps send none.

Rules (checked in the app and again on the server): 2 to 24 characters, letters, digits, space and `_ - .` only; no web or e-mail addresses; a few names are reserved (admin, onair, support and similar) and a short list of offensive words is refused. A name the server refuses is dropped: your data is still accepted, anonymously, and the reply says why.

Server side: the name lives with the track only while the track is on the map (minutes to hours, as for any track). It is not written to logs, no IP address is stored, and the upload rate limits are unchanged. Search: `GET /api/by/<name>` (case-insensitive prefix, `?exact=1` for exact, at most 200 tracks).
