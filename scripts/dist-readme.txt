OOC Radio for FH6
=================

Thanks for grabbing this. It's a free, open-source mod that drops a
brand new station into Forza Horizon 6's radio dial, fed by the "Out of
Character Radio" stream (radio.oocradio.com). The game treats it like
every other station: it ducks for menus, follows your in-game volume
slider, and fades on the loading screen. When a real presenter is on
air, the radio dial shows their name/avatar instead of plain track
info.

There is no web dashboard -- everything is configured by hand-editing
one text file, and playback is fully automatic once that's set.


Getting it running
~~~~~~~~~~~~~~~~~~

Make sure FH6 isn't open first. Then drop the contents of this archive
straight into the folder that contains forzahorizon6.exe. Depending on
where you installed the game, that'll look like one of:

    Steam      ->  ...\steamapps\common\ForzaHorizon6
    Xbox app   ->  ...\XboxGames\Forza Horizon 6\Content

Let Windows overwrite when it asks. Heads-up: some antivirus tools dunk
on the bundled version.dll because of how the mod hooks into the game.
If yours yeets the file, add the FH6 folder to its exclusions list
and re-extract.

Next, open fh6-radio\config.toml in a text editor and paste your OOC
Radio API key into the api_key field under [ooc_radio]. Generate one
from the "Developer" section of https://oocradio.com/ if you
don't have one yet. Without a key, the stream still plays but title/
artist/live-presenter info won't show.

Once the files are in place and the key is set, launch the game and
head into Settings > Audio. Two switches matter:

    Streamer Mode  ->  ON     (the new station only shows up with
                                this enabled)
    Radio DJ       ->  OFF    (otherwise the in-game DJ chimes in
                                over your tracks)

Now cycle the radio stations in-game until you land on the new one.
The mod's audio only goes out while that station is the active one.
Flip to another station and it stops broadcasting.


Pulling it back out
~~~~~~~~~~~~~~~~~~~

Two things to remove from the FH6 install folder: version.dll, and the
fh6-radio/ folder sitting next to it. After that, hit "Verify integrity
of game files" (Steam) or "Repair" (Xbox app / MS Store) and the
patched game assets get pulled back to vanilla.


About the project
~~~~~~~~~~~~~~~~~

This mod is a hobby project released under GPLv3, forked from
g0ldyy's fh6-universal-radio (github.com/g0ldyy/fh6-universal-radio),
which was itself inspired by "Big John"'s closed-source FH6 Spotify
radio mod. See NOTICE for the full attribution chain.

Unofficial fan project. Nothing here is affiliated with, endorsed by,
or connected to Turn 10 Studios, Playground Games, Xbox Game Studios,
Microsoft, or OOC Radio. Forza Horizon, Forza Motorsport, and all other
names dropped above belong to their respective owners. Provided as-is,
no warranty, use at your own risk.
