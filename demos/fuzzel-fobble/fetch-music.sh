#!/bin/bash
# Fetch the Notebook for Anna Magdalena Bach into music/notebook/, so the game
# has a piece per level instead of one minuet on a loop.
#
# Both run scripts call this before building. It is not required: nothing here
# is committed, nothing here is needed to play, and a machine with no network
# gets the minuet on every level and no error. That is the same bargain
# run-raylib.sh makes with raylib itself — fetched on demand, cached, ignored
# by git, deletable at any time.
#
#     ./fetch-music.sh            fetch what is missing, quietly if nothing is
#     ./fetch-music.sh --force    re-fetch everything
#
# ------------------------------------------------------------------
# Where this comes from
# ------------------------------------------------------------------
#
# The Mutopia Project (mutopiaproject.org) typesets public-domain scores in
# LilyPond and publishes the PDF, the LilyPond source, and a MIDI rendering of
# each. The MIDI is a by-product of engraving the score rather than a
# performance: no dynamics, no rubato, two or three tracks of exactly what is
# on the page. For an OPL2 that is the good case — what arrives is the notes,
# and the chip is what makes them a sound.
#
# Twenty of the notebook's forty-two entries are there, listed below in the
# notebook's own order. Nineteen are marked Public Domain; two are Creative
# Commons Attribution-ShareAlike, credited in the CREDITS file this writes.
# Neither licence is strained by any of this: the files are downloaded by the
# person running the game, onto their own machine, and never redistributed.
#
# No. 4 — the Minuet in G, BWV Anh. 114 — is deliberately *not* fetched. The
# game brings its own, arranged across five channels in tools/mkmid.fc, and it
# is level 1. Hearing the plain engraving of it again at level 2 would be a
# worse start than any of the pieces below.
set -u

cd "$(dirname "$0")"

DEST="music/notebook"
BASE="https://www.mutopiaproject.org/ftp/BachJS"
FORCE=""
[ "${1:-}" = "--force" ] && FORCE=1

# Notebook number, Mutopia path, and the title as the game shows it. The
# titles are upper case with no accent and no flat sign because art.fc's font
# is a 3x5 grid of A-Z, 0-9 and five punctuation marks — the display is part
# of the data, so the mangling happens here rather than in the game.
#
# Numbers 13 and 34+ are why the count is twenty rather than the notebook's
# forty-two: entry 13 is two settings and gets two lines, and most of what is
# missing is either music Mutopia has not typeset or the vocal half of the
# book, which is a voice and a figured bass rather than a keyboard piece.
PIECES=(
    "03|BWVAnh113/anna-magdalena-03|MINUET IN F - BWV ANH. 113"
    "05|BWVAnh115/anna-magdalena-05|MINUET IN G MINOR - BWV ANH. 115"
    "07|BWVAnh116/anna-magdalena-07|MINUET IN G - BWV ANH. 116"
    "08|BWVAnh117b/BWV-117b|POLONAISE IN F - BWV ANH. 117B"
    "09|BWVAnh118/BWV-118|MINUET IN B FLAT - BWV ANH. 118"
    "10|BWVAnh119/BWV-119|POLONAISE IN G MINOR - BWV ANH. 119"
    "11|BWVAnh691/BWV-691|WER NUR DEN LIEBEN GOTT - BWV 691"
    "12|BWV510/BWV-510|GIB DICH ZUFRIEDEN IN F - BWV 510"
    "13|BWV511/BWV-511|GIB DICH ZUFRIEDEN IN D MINOR - BWV 511"
    "13|BWV512/BWV-512|GIB DICH ZUFRIEDEN IN E MINOR - BWV 512"
    "14|BWVAnh120/BWV-120|MINUET IN A MINOR - BWV ANH. 120"
    "15|BWVAnh121/BWV-121|MINUET IN C MINOR - BWV ANH. 121"
    "20|BWV515/anna-magdalena-20a|SO OFT ICH MEINE TOBACKSPFEIFE - BWV 515"
    "22|BWVAnh126/anna-magdalena-22|MUSETTE IN D - BWV ANH. 126"
    "23|BWVAnh127/BWV-127|MARCH IN E FLAT - BWV ANH. 127"
    "24|BWVAnh128/BWV-128|POLONAISE IN D MINOR - BWV ANH. 128"
    "25|BWV508/BistDuBeiMir|BIST DU BEI MIR - BWV 508"
    "26|BWV988/bwv-988-aria|ARIA IN G - BWV 988"
    "32|BWVAnh131/air|AIR IN F - BWV ANH. 131"
    "33|BWV516/BWV-516|WARUM BETRUBST DU DICH - BWV 516"
)

if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fsSL --max-time 30 -o "$1" "$2"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -q -T 30 -O "$1" "$2"; }
else
    echo "fetch-music: no curl or wget — the game will play its own minuet" >&2
    exit 1
fi

mkdir -p "$DEST"

# The manifest is written from scratch on every run and lists only the files
# that are actually on disk when it is written. A half-finished fetch (network
# dropped, Ctrl-C, disk full) therefore leaves a shorter playlist rather than
# a playlist naming a file that isn't there — the game trusts this file.
MANIFEST="$DEST/playlist.txt"
: > "$MANIFEST.tmp"

n=0
got=0
missing=0
fetched=0
announced=0
for entry in "${PIECES[@]}"; do
    IFS='|' read -r num path title <<< "$entry"
    n=$((n + 1))
    file="$(printf '%02d' "$n")-$(basename "$path").mid"

    if [ -n "$FORCE" ] || [ ! -s "$DEST/$file" ]; then
        if [ "$announced" -eq 0 ]; then
            echo "Fetching the Notebook for Anna Magdalena Bach into $DEST/ ..."
            announced=1
        fi
        # Mutopia gives each piece its own directory and names every file in
        # it after that directory, so the last path component appears twice.
        if fetch "$DEST/$file.part" "$BASE/$path/$(basename "$path").mid" 2>/dev/null; then
            mv -f "$DEST/$file.part" "$DEST/$file"
            fetched=$((fetched + 1))
        else
            # A piece that will not download costs that piece and nothing
            # else: it drops out of the playlist and the levels close up.
            rm -f "$DEST/$file.part"
            missing=$((missing + 1))
            continue
        fi
    fi

    # `notebook no.` is carried through so the game can say where in the book
    # a piece sits, which is the one fact its own filename cannot tell you.
    printf '%s|%s|%s\n' "$file" "$num" "$title" >> "$MANIFEST.tmp"
    got=$((got + 1))
done

mv -f "$MANIFEST.tmp" "$MANIFEST"

cat > "$DEST/CREDITS" <<'CREDITS'
Music in this directory is fetched from the Mutopia Project by
demos/fuzzel-fobble/fetch-music.sh. Nothing here is part of the fc-lang
repository and nothing here is committed; delete the directory to remove it.

    https://www.mutopiaproject.org/

The music is from the Notebook for Anna Magdalena Bach (1725), which is public
domain. The editions are Mutopia's, typeset in LilyPond and published with the
MIDI rendering these files are. Their typesetters:

    Allen Garvin        BWV Anh. 113, 115, 116, 126; BWV 515
    Steven McDougall    BWV Anh. 117b, 118, 119, 120, 121, 127, 128;
                        BWV 510, 511, 512, 516, 691
    Shamim Mohamed      BWV 508
    JD Erickson         BWV 988/1
    Anonymous           BWV Anh. 131

Two editions carry a licence rather than a public-domain dedication, and both
ask for the credit given above:

    BWV Anh. 131 (Air)          CC Attribution-ShareAlike 2.5
    BWV 988/1 (Aria)            CC Attribution-ShareAlike 3.0

The Minuet in G, BWV Anh. 114, is not here. The game ships its own arrangement
of it in music/minuet.mid, written in demos/fuzzel-fobble/tools/mkmid.fc.
CREDITS

if [ "$fetched" -gt 0 ]; then
    echo "Fetched $fetched of $n pieces (public domain / CC-BY-SA, see $DEST/CREDITS)"
fi
if [ "$missing" -gt 0 ]; then
    echo "fetch-music: $missing piece(s) did not download — the rest still play" >&2
fi
if [ "$got" -eq 0 ]; then
    echo "fetch-music: nothing downloaded — the game will play its own minuet" >&2
    exit 1
fi
exit 0
