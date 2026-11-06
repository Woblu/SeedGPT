"""Turn a sentence into a query, on this machine, with no AI key.

The prompt box is the front door, and a front door that opens onto "paste an API
key from another website" loses most of the people who reach it. This covers the
requests people type first: structures, biomes, ores, chest loot, distances,
"near spawn", "no ocean", "this or that". Anything richer is what the optional
AI path is for.

The one rule that matters: nothing is dropped quietly. A search for half of what
was asked reads as a working result for the whole of it, and a search for the
opposite is worse. So a word only disappears if it is on the short list of words
that cannot change a meaning, or a rule below understood it. Everything else comes
back in `ignored`, and the page asks before running. When in doubt, this file
should refuse rather than guess: refusing costs a click, and guessing costs someone
an evening flying to a place that is not there.

Names come from the engine's vocabulary for the chosen version, so a structure
that version does not have cannot be asked for.
"""

import math
import re
from dataclasses import dataclass, field
from typing import List, Optional


class NotUnderstood(Exception):
    """Nothing in the text could be turned into a condition."""


# Spoken names, longest first at match time. Keys of the engine's vocabulary are
# added automatically, so these are only the names people use that differ.
STRUCTURES = {
    "woodland mansion": "mansion", "mansion": "mansion",
    "village": "village",
    "ocean monument": "monument", "monument": "monument", "guardian temple": "monument",
    "desert temple": "desert_pyramid", "desert pyramid": "desert_pyramid", "pyramid": "desert_pyramid",
    "jungle temple": "jungle_temple", "jungle pyramid": "jungle_temple",
    "swamp hut": "swamp_hut", "witch hut": "swamp_hut", "witch's hut": "swamp_hut",
    "witches hut": "swamp_hut",
    "igloo": "igloo", "shipwreck": "shipwreck", "wreck": "shipwreck",
    "pillager outpost": "outpost", "outpost": "outpost",
    "ancient city": "ancient_city", "deep dark city": "ancient_city",
    "ruined portal": "ruined_portal", "nether ruined portal": "ruined_portal_nether",
    "trail ruins": "trail_ruins", "trail ruin": "trail_ruins",
    "trial chambers": "trial_chambers", "trial chamber": "trial_chambers",
    "buried treasure": "buried_treasure", "treasure": "buried_treasure",
    "ocean ruins": "ocean_ruin", "ocean ruin": "ocean_ruin",
    "mineshaft": "mineshaft", "mine shaft": "mineshaft",
    "amethyst geode": "geode", "geode": "geode",
    "nether fortress": "fortress", "fortress": "fortress",
    "bastion remnant": "bastion", "bastion": "bastion",
    "end city": "end_city",
}

# Say what is special about a structure in the words people use for it.
VARIANTS = {
    "zombie village": ("village", {"abandoned": True}),
    "abandoned village": ("village", {"abandoned": True}),
    "igloo with a basement": ("igloo", {"basement": True}),
    "igloo with basement": ("igloo", {"basement": True}),
    "end ship": ("end_city", {"ship": True}),
    "elytra": ("end_city", {"ship": True}),
    "giant ruined portal": ("ruined_portal", {"giant": True}),
    "giant portal": ("ruined_portal", {"giant": True}),
    "surface ruined portal": ("ruined_portal", {"surface": True}),
}

# Biomes that are also landscapes: said plainly, they mean the whole family.
FAMILIES = {"badlands", "jungle", "taiga", "forest", "savanna", "swamp", "ocean", "mountain"}
BIOME_NAMES = {
    "mesa": ("badlands", True), "badlands": ("badlands", True),
    "mushroom island": ("mushroom_fields", False), "mushroom biome": ("mushroom_fields", False),
    "mushroom": ("mushroom_fields", False),
    "mountains": ("mountain", True), "mountain": ("mountain", True),
    "cherry": ("cherry_grove", False), "cherry blossoms": ("cherry_grove", False),
    "cherry forest": ("cherry_grove", False),
}

ORES = {
    "diamond": "diamond", "emerald": "emerald", "gold": "gold", "iron": "iron",
    "coal": "coal", "copper": "copper", "lapis": "lapis", "lapis lazuli": "lapis",
    "redstone": "redstone", "ancient debris": "ancient_debris", "netherite": "ancient_debris",
    "nether quartz": "quartz", "quartz": "quartz", "nether gold": "nether_gold",
}
NETHER_ORES = {"quartz", "ancient_debris", "nether_gold"}

# Chest items, by what people call them. An item only counts as loot when it
# follows a structure ("a pyramid with 2 diamonds"); on its own it is an ore.
ITEMS = {
    "enchanted golden apple": "enchanted_golden_apple", "god apple": "enchanted_golden_apple",
    "notch apple": "enchanted_golden_apple", "golden apple": "golden_apple",
    "diamond horse armor": "diamond_horse_armor", "golden horse armor": "golden_horse_armor",
    "iron horse armor": "iron_horse_armor",
    "diamond": "diamond", "emerald": "emerald", "gold": "gold_ingot", "gold ingot": "gold_ingot",
    "iron": "iron_ingot", "iron ingot": "iron_ingot", "saddle": "saddle", "name tag": "name_tag",
}

NUMBER_WORDS = {w: str(i) for i, w in enumerate(
    "zero one two three four five six seven eight nine ten eleven twelve".split())}

# Words that cannot change what is being asked. This list is the whole safety of
# this file, so it is deliberately short: "without", "near", "more", "than",
# "under" and "away" change a meaning, so they are absent, and they only vanish
# when a rule has actually understood them.
FILLER = set("""a an the and with that which has have having is are be it its there where
    seed seeds world worlds minecraft java edition find show get give want need looking look
    search searching please me my i we us can you could would like also both either them
    their one ones some any of to in on at for by from this will should do does
    good nice cool great best awesome perfect amazing just only very really all""".split())

DEFAULT_WITHIN = {"structure": 2000, "biome": 800, "ore": 64, "slime": 128,
                  "stronghold": 3000, "loot": 3000}
NEAR, ADJACENT, INSIDE, AT_SPAWN = 400, 200, 64, 96


@dataclass
class Item:
    kind: str                # structure, biome, ore, eyes, island, slime, stronghold, loot, item
    start: int
    end: int
    fields: dict
    within: Optional[int] = None  # blocks, always; `chunks` only sets the shape
    chunks: bool = False
    explicit: bool = False        # `within` was said, not assumed
    of: Optional[str] = None      # "spawn", "origin", or another Item through .parent
    parent: Optional["Item"] = None
    negate: bool = False
    group: Optional[int] = None   # items sharing a value are "this OR that"
    id: str = ""
    near_prev: bool = False


@dataclass
class Sentence:
    text: str
    used: List[tuple] = field(default_factory=list)   # spans that were understood

    def use(self, start, end):
        self.used.append((start, end))

    def blank(self, start, end):
        """Take a span out of the sentence, so no later rule can read it."""
        self.text = self.text[:start] + " " * (end - start) + self.text[end:]

    def leftovers(self):
        """Words no rule accounted for, in their original order."""
        chars = list(self.text)
        for start, end in self.used:
            chars[start:end] = " " * (end - start)
        words = re.findall(r"[a-z0-9.'_-]+", "".join(chars))
        return [w for w in words if w.strip("'.-") and w.strip("'.-") not in FILLER]


def _normalise(text: str) -> str:
    s = text.lower().replace("’", "'").replace("–", "-").replace("—", "-")
    s = re.sub(r"(?<=\d),(?=\d{3}\b)", "", s)                 # 1,000 -> 1000
    s = re.sub(r"\b(\d+(?:\.\d+)?)k\b", lambda m: str(int(float(m.group(1)) * 1000)), s)
    s = re.sub(r"\b(" + "|".join(NUMBER_WORDS) + r")\b(?=\s+(?:\w+[- ])?(?:eye|diamond|emerald|"
               r"gold|iron|apple|slime|block|chunk))", lambda m: NUMBER_WORDS[m.group(1)], s)
    return re.sub(r"\s+", " ", s).strip()


def _alternation(names) -> str:
    return "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True))


def _vocab_names(vocab: dict):
    """Structure and biome spoken forms for this version -> engine ids."""
    have_s = set(vocab.get("structures", {}))
    structures = {k: v for k, v in STRUCTURES.items() if v in have_s}
    variants = {k: v for k, v in VARIANTS.items() if v[0] in have_s}
    biomes = {}
    for b in vocab.get("biomes", {}):
        biomes[b.replace("_", " ")] = (b, b in FAMILIES)
    for k, (b, fam) in BIOME_NAMES.items():
        if b in vocab.get("biomes", {}) or (fam and b == "mountain"):
            biomes[k] = (b, fam)
    return structures, variants, biomes


def _find_items(s: Sentence, vocab: dict) -> List[Item]:
    structures, variants, biomes = _vocab_names(vocab)
    items, taken = [], []

    def claim(pattern, make):
        for m in re.finditer(pattern, s.text):
            if any(m.start() < e and m.end() > b for b, e in taken):
                continue
            item = make(m)
            if item:
                taken.append((m.start(), m.end()))
                s.use(item.start, item.end)
                items.append(item)

    # Longest and most specific first, so "desert temple" is not a desert biome
    # with a stray word after it, and "jungle temple" not a jungle.
    claim(r"\b(\d+)[- ]?eyes?(?:[- ](?:end )?portal)?\b(?:\s+(?:already\s+)?in\s+(?:the\s+|an?\s+)?(?:end\s+)?portal)?",
          lambda m: Item("eyes", m.start(), m.end(), {"eyes": int(m.group(1))}))
    claim(r"\b(?:(\d+)\s+)?slime chunks?\b",
          lambda m: Item("slime", m.start(), m.end(), {"slime": int(m.group(1) or 4)}))
    claim(r"(?<!mushroom )\b(?:survival )?island(?: spawn)?\b(?! biome)|\bstranded\b|\bsurrounded by (?:ocean|water)\b",
          lambda m: Item("island", m.start(), m.end(), {"island": True}))
    claim(r"\b(exposed|shallow)\s+stronghold\b",
          lambda m: Item("stronghold", m.start(), m.end(), {"stronghold": m.group(1)}))
    claim(r"\bstronghold\b", lambda m: Item("stronghold", m.start(), m.end(), {"stronghold": True}))
    claim(r"\b(" + _alternation(variants) + r")s?\b",
          lambda m: Item("structure", m.start(), m.end(),
                         dict({"structure": variants[m.group(1)][0]}, **variants[m.group(1)][1])))
    claim(r"\b(" + _alternation(structures) + r")s?\b",
          lambda m: Item("structure", m.start(), m.end(), {"structure": structures[m.group(1)]}))
    claim(r"\b(" + _alternation(set(ITEMS) | set(ORES)) + r")s?\b", lambda m: _item_or_ore(s, m))
    claim(r"\b(" + _alternation(biomes) + r")s?\b(?: biome)?",
          lambda m: _biome(biomes[m.group(1)], m))
    return sorted(items, key=lambda i: i.start)


def _biome(name_family, m) -> Item:
    name, family = name_family
    fields = {"biome": name}
    if family:
        fields["family"] = True
    return Item("biome", m.start(), m.end(), fields)


def _item_or_ore(s: Sentence, m) -> Optional[Item]:
    """An item name: loot if a structure just came before it, else an ore."""
    word = m.group(1)
    before = s.text[:m.start()]
    count = re.search(r"(?:at least |over |more than )?(\d+)\s+(?:\w+\s+)?$", before)
    start = count.start() if count else m.start()
    n = int(count.group(1)) if count else 0
    in_chest = re.search(r"\b(?:with|has|have|having|containing|holding|contains)\s+"
                         r"(?:at least |over |more than )?(?:\d+\s+|an?\s+)?(?:\w+\s+)?$", before)
    if word in ORES and (not in_chest or word not in ITEMS):
        return Item("ore", start, m.end(), {"ore": ORES[word], "count": n or 4})
    return Item("item", start, m.end(), {"item": ITEMS[word], "word": word, "count": n})


def _version(s: Sentence) -> Optional[str]:
    """Find the version and take it out of the sentence, or "in 1.20" reads as a
    distance of one. Only versions that exist: "1.5 chunks" is a length."""
    m = re.search(r"(?:\b(?:in|on|for|version|mc|minecraft|java)\s+)*"
                  r"\bv?(1\.(?:1[2-9]|2\d)(?:\.\d{1,2})?|2\d\.\d{1,2}(?:\.\d{1,2})?)\b"
                  r"(?!\s*(?:blocks?|chunks?|k\b))", s.text)
    if not m:
        return None
    s.blank(m.start(), m.end())
    return m.group(1)


# How far. A number needs a unit or a word that makes it a distance.
RADIUS = re.compile(r"(?:(within|in|under|less than|at most|up to|no more than|about|around|<)\s+)?"
                    r"(\d+(?:\.\d+)?)\s*(blocks?|chunks?|b\b|m\b)?")
# The same shape meaning the opposite: "at least 1000 blocks from spawn" is not
# "within 1000". The engine has no minimum distance, so these are refused.
FAR = re.compile(r"\b(?:at least|more than|over|further than|farther than|beyond|outside(?: of)?|"
                 r"minimum of)\s+\d+(?:\.\d+)?(?:\s*(?:blocks?|chunks?))?(?:\s+(?:away\s+)?(?:from|of)\b)?")
# A link from the item on the left to the item on the right, ending the gap.
RELATE = re.compile(r"\b(?:right\s+|directly\s+|just\s+|very\s+)?"
                    r"(next to|next door to|adjacent to|beside|touching|by|near|close to|"
                    r"around|away from|from|of|in|inside|on|under|underneath|below|above)"
                    r"\s*(?:an?\s+|the\s+|some\s+)?$")
REF_WORDS = re.compile(r"\b(?:the\s+)?(spawn(?:ing|s| point)?|origin|world origin|center|centre|"
                       r"middle(?: of the world)?|0,\s*0)\b")
NEARBY_TAIL = re.compile(r"\b(nearby|close by|close(?! to)|near(?:by)? (?:it|there)|next to it|"
                         r"by it|around it|beside it)\b")
CLOSE = ((ADJACENT, re.compile(r"\b(?:right\s+)?(?:next to|next door to|adjacent to|beside|touching)\b")),
         (NEAR, re.compile(r"\b(?:right\s+)?(?:near|nearby|close to|close by|around|by)\b")),
         (AT_SPAWN, re.compile(r"\b(?:right\s+)?(?:at|on|in|inside)\b|\bright\b")))
NO_RADIUS_WORDS = ("of", "from", "away from")


def _closeness(s: Sentence, start, text) -> Optional[int]:
    """How close "near spawn" or "right at spawn" means, consuming the words."""
    for distance, pattern in CLOSE:
        m = pattern.search(text)
        if m:
            s.use(start + m.start(), start + m.end())
            return distance
    return None


def _link_distance(word: str) -> int:
    if word in ("in", "inside", "on", "under", "underneath", "below", "above"):
        return INSIDE
    if word in ("next to", "next door to", "adjacent to", "beside", "touching", "by"):
        return ADJACENT
    return NEAR


def _read_gap(s: Sentence, start, end, item: Item, nxt: Optional[Item], refused: list):
    """What the words between two items say about the one on the left."""
    gap = s.text[start:end]

    far = FAR.search(gap)
    if far:
        refused.append(far.group(0).strip())
        s.use(start + far.start(), start + far.end())
        return

    radius = None
    for candidate in RADIUS.finditer(gap):
        if candidate.group(1) or candidate.group(3):
            radius = candidate
            break
    if radius:
        chunks = bool(radius.group(3) and radius.group(3).startswith("chunk"))
        item.within = math.ceil(float(radius.group(2)) * (16 if chunks else 1))
        item.chunks, item.explicit = chunks, True
        s.use(start + radius.start(), start + radius.end())

    rel = RELATE.search(gap) if nxt is not None else None
    if rel and rel.group(1) in NO_RADIUS_WORDS and not radius:
        rel = None      # "of" and "from" mean nothing without a distance to measure

    place = REF_WORDS.search(gap)
    if place:
        word = place.group(1)
        item.of = "spawn" if word.startswith("spawn") else "origin"
        s.use(start + place.start(), start + place.end())
        if not radius:
            head = gap[:rel.start()] if rel else gap
            head = head[:place.start()] + " " * (place.end() - place.start()) + head[place.end():]
            near = _closeness(s, start, head)
            if near:
                item.within, item.explicit = near, True

    tail = NEARBY_TAIL.search(gap)
    if tail and not place:
        item.near_prev = True
        s.use(start + tail.start(), start + tail.end())

    if not rel:
        return
    word = rel.group(1)
    s.use(start + rel.start(), end)
    # A link needs one thing on each end. An `of` cannot point into a group, and
    # linking only the nearest member would read as linking all of them.
    if item.group is not None or nxt.group is not None:
        refused.append(f"{word} {s.text[nxt.start:nxt.end]}")
    elif place:
        # One condition has one `of`, and that went to the place: the other
        # thing is measured from this one instead.
        nxt.parent = item
        nxt.within = _link_distance(word)
    else:
        item.parent = nxt
        if item.within is None:
            item.within = _link_distance(word)


def _assign_ids(items: List[Item]):
    counts = {}
    for it in items:
        loot = it.fields.get("loot")
        base = (it.fields.get("structure") or it.fields.get("biome") or it.fields.get("ore")
                or (loot and loot["structure"]) or it.kind)[:12]
        counts[base] = counts.get(base, 0) + 1
        it.id = base if counts[base] == 1 else f"{base}{counts[base]}"


def _dimension(it: Item, vocab: dict) -> str:
    f = it.fields
    if "structure" in f:
        return vocab.get("structures", {}).get(f["structure"], "overworld")
    if "loot" in f:
        return vocab.get("structures", {}).get(f["loot"]["structure"], "overworld")
    if "biome" in f:
        return vocab.get("biomes", {}).get(f["biome"], "overworld")
    return "nether" if f.get("ore") in NETHER_ORES else "overworld"


def _condition(it: Item, vocab: dict, notes: list) -> dict:
    c = {"id": it.id}
    c.update({k: v for k, v in it.fields.items() if k != "word"})
    if it.negate:
        c["not"] = True
    if it.kind == "eyes":
        return c
    if it.kind == "island":
        c["of"] = "spawn"
        c["pct"], c["within"] = 75, it.within or 256
        return c
    within = it.within if it.within is not None else DEFAULT_WITHIN.get(it.kind, 1000)
    # Ore counts are per chunk and capped well below a structure's reach, so
    # "near" means something closer for ore. A number that was typed stands.
    if it.kind == "ore" and not it.explicit:
        within = min(within, DEFAULT_WITHIN["ore"])
    if it.chunks:
        c["metric"] = "chebyshev"
    c["within"] = within
    c["of"] = it.parent.id if it.parent else (it.of or "origin")
    dim = _dimension(it, vocab)
    if c["of"] == "spawn" and dim != "overworld":
        # World spawn is in the overworld, so this could only ever be refused.
        c["of"] = "origin"
        notes.append(f"{dim} distances are measured from 0,0, not from spawn")
    return c


def _group_or(s: Sentence, items: List[Item]):
    """"this or that" puts both in one group, and so does "this, that or other"."""
    group = 0
    for i, (a, b) in enumerate(zip(items, items[1:])):
        if not re.fullmatch(r"\s*(?:,\s*)?(?:or|either)\s+(?:an?\s+|the\s+|some\s+)?",
                            s.text[a.end:b.start]):
            continue
        if a.group is None:
            group += 1
            a.group = group
            for j in range(i, 0, -1):
                gap = s.text[items[j - 1].end:items[j].start]
                if not re.fullmatch(r"\s*,\s*(?:an?\s+|the\s+|some\s+)?", gap):
                    break
                items[j - 1].group = group
                s.use(items[j - 1].end, items[j].start)
        b.group = a.group
        s.use(a.end, b.start)


def _negate(s: Sentence, items: List[Item]):
    """"no ocean", "without a village". "no ocean or river" means neither, which
    is two absences that must both hold, not one group."""
    for it in items:
        m = re.search(r"\b(?:no|not|without|never|avoid|but no|and no)\s+(?:an?\s+|any\s+|the\s+)?$",
                      s.text[:it.start])
        if not m or it.kind not in ("structure", "biome"):
            continue
        it.negate = True
        s.use(m.start(), it.start)
        if it.group is not None:
            members = [o for o in items if o.group == it.group]
            if it is members[0]:
                for o in members:
                    if o.kind in ("structure", "biome"):
                        o.negate = True
                    o.group = None


def _take_chest_items(s: Sentence, items: List[Item], loot: dict, notes: list, refused: list):
    """A chest item belongs to the structure before it."""
    kept = []
    for it in items:
        if it.kind != "item":
            kept.append(it)
            continue
        host = kept[-1] if kept and kept[-1].kind == "structure" else None
        have = loot.get(host.fields["structure"], []) if host else []
        if host and it.fields["item"] in have:
            host.kind = "loot"
            host.fields = {"loot": {"structure": host.fields["structure"], "item": it.fields["item"],
                                    "count": it.fields["count"] or 1}}
            if host.group is None:
                host.group = it.group
            s.use(host.end, it.end)
        elif host:
            # Unread, not just a note: the structure alone would be a confident
            # answer to a different question.
            notes.append(f"{host.fields['structure'].replace('_', ' ')} chests do not hold {it.fields['word']}")
            refused.append(it.fields["word"])
        elif it.fields["word"] in ORES:
            it.kind, it.fields = "ore", {"ore": ORES[it.fields["word"]], "count": it.fields["count"] or 4}
            kept.append(it)
        else:
            notes.append(f"{it.fields['word']} only makes sense in a chest: name the structure it is in")
            refused.append(it.fields["word"])
    return kept


def _link_with(s: Sentence, items: List[Item], refused: list):
    """"a mansion with a village nearby", "a mansion with a jungle"."""
    for i, it in enumerate(items):
        prev = items[i - 1] if i else None
        if it.near_prev and prev is not None and it.parent is None:
            if it.group is not None or prev.group is not None:
                refused.append(f"nearby {s.text[it.start:it.end]}")
            else:
                it.parent = prev
                if it.within is None:
                    it.within = NEAR
        joined = prev is not None and re.search(r"\b(?:with|has|have|having)\b", s.text[prev.end:it.start])
        if joined and it.parent is None and not it.of and prev.group is None \
                and prev.kind in ("structure", "loot") and it.kind in ("structure", "biome"):
            for member in ([o for o in items if o.group == it.group] if it.group is not None else [it]):
                member.parent, member.within = prev, member.within or NEAR


def _turn_around_links(items: List[Item]):
    """A biome or an ore has no single position to measure from, so "a mansion in
    a jungle" is a jungle around the mansion. Turn the link round, keeping the
    distance, and keep whatever the biome was linked to as well."""
    for it in items:
        p = it.parent
        if p is None or p.kind not in ("biome", "ore") or it.kind not in ("structure", "loot", "stronghold"):
            continue
        further, further_within = p.parent, p.within
        p.parent, p.within, p.chunks, p.explicit = it, it.within, it.chunks, it.explicit
        it.parent, it.within, it.chunks, it.explicit = further, further_within, False, False


def _share_place(items: List[Item]):
    """"a mansion and a village within 1000 blocks of spawn": a place named once
    covers what named none, and a distance that closes the sentence covers the
    items before it."""
    place = next((it for it in reversed(items) if it.of), None)
    last = items[-1]
    for it in items:
        if it.parent is None and it.of is None and place:
            it.of = place.of
        if it is not last and it.parent is None and it.within is None and last.parent is None \
                and last.within is not None and last.explicit:
            it.within, it.chunks, it.explicit = last.within, last.chunks, True


def _eyes(items: List[Item], notes: list, refused: list) -> List[Item]:
    """"A stronghold with 8 eyes": the eye count already is that stronghold, and
    it always means the nearest one, so a distance cannot be honoured."""
    if not any(it.kind == "eyes" for it in items):
        return items
    kept = []
    for it in items:
        placed = it.within is not None or it.of or it.parent
        if it.kind == "stronghold" and it.fields["stronghold"] is True:
            if placed:
                refused.append("the distance to that stronghold")
            continue
        if it.kind == "eyes" and placed:
            refused.append("the distance to that stronghold")
        kept.append(it)
    if len(refused):
        notes.append("an eye count always means the stronghold nearest spawn")
    return kept


def parse(text: str, version: str, vocab: dict, loot: Optional[dict] = None) -> dict:
    """{"query": ..., "notes": ..., "ignored": [...]} for a sentence.

    `version` is the one chosen in the page, and a version named in the sentence
    overrides it, so the vocabulary is the caller's to match: see want_version().
    """
    s = Sentence(_normalise(text))
    _version(s)
    large = re.search(r"\blarge biomes?\b", s.text)
    if large:
        s.use(large.start(), large.end())
    items = _find_items(s, vocab)
    if not items:
        raise NotUnderstood(text)

    notes, refused = [], []
    _group_or(s, items)
    _negate(s, items)
    items = _take_chest_items(s, items, (loot or {}).get("structures", {}), notes, refused)
    if not items:
        raise NotUnderstood(text)

    for i, it in enumerate(items):
        nxt = items[i + 1] if i + 1 < len(items) else None
        _read_gap(s, it.end, nxt.start if nxt else len(s.text), it, nxt, refused)
    _read_gap(s, 0, items[0].start, items[0], None, refused)
    _link_with(s, items, refused)
    _turn_around_links(items)
    _share_place(items)
    items = _eyes(items, notes, refused)
    if not items:
        raise NotUnderstood(text)

    _assign_ids(items)
    groups, conditions = {}, []
    for it in items:
        c = _condition(it, vocab, notes)
        if it.group is None:
            conditions.append(c)
        elif it.group in groups:
            groups[it.group]["any"].append(c)
        else:
            groups[it.group] = {"id": f"any{it.group}", "any": [c]}
            conditions.append(groups[it.group])

    query = {"version": version, "conditions": conditions}
    if large:
        query["large_biomes"] = True
    return {"query": query, "notes": "; ".join(dict.fromkeys(notes)),
            "ignored": refused + s.leftovers()}


def want_version(text: str) -> Optional[str]:
    """The version a sentence names, if any: the vocabulary has to be that one's."""
    return _version(Sentence(_normalise(text)))
