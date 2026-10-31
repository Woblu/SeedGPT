"""Turn a sentence into a query, on this machine, with no AI key.

The prompt box is the front door, and a front door that opens onto "paste an API
key from another website" loses most of the people who reach it. This covers the
requests people type first: structures, biomes, ores, chest loot, distances,
"near spawn", "no ocean", "this or that". Anything richer is what the optional
AI path is for.

The one rule that matters: nothing is dropped quietly. A search for half of what
was asked reads as a working result for the whole of it, so every word that is
not understood comes back in `ignored`, and the page asks before running.

Names come from the engine's vocabulary for the chosen version, so a structure
that version does not have cannot be asked for.
"""

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

# Words that carry no condition, so they cannot be "not understood".
FILLER = set("""a an the and any some of to in on at for by with without that which who
    has have having had is are be it its there where seed seeds world worlds minecraft java
    edition find show get give want need looking look search searching please me my i we
    us can you could would like also both them their one ones good nice cool great best
    awesome perfect amazing starting start spawning spawns close nearby near right next
    around from away blocks block chunks chunk within than less under more most least
    at all just only very really from this version be will should do does""".split())

DEFAULT_WITHIN = {"structure": 2000, "biome": 800, "ore": 64, "slime": 128,
                  "stronghold": 3000, "loot": 3000}
NEAR, ADJACENT, INSIDE = 400, 200, 64


@dataclass
class Item:
    kind: str                # structure, biome, ore, eyes, island, slime, stronghold
    start: int
    end: int
    fields: dict
    within: Optional[int] = None
    chunks: bool = False
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

    def leftovers(self):
        """Words no rule accounted for, in their original order."""
        chars = list(self.text)
        for start, end in self.used:
            chars[start:end] = " " * (end - start)
        words = re.findall(r"[a-z0-9.'_-]+", "".join(chars))
        return [w for w in words if w.strip("'.-") not in FILLER and w.strip("'.-")]


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
                s.use(m.start(), m.end())
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
    claim(r"\b(" + _alternation(ITEMS) + r")s?\b(?=.*)",
          lambda m: _item_or_ore(s, m))
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
    fields = {"item": ITEMS[word], "word": word, "count": int(count.group(1)) if count else 0}
    if word in ORES and not re.search(r"\b(?:with|has|have|having|containing|holding|contains)\s+"
                                      r"(?:at least |over |more than )?(?:\d+\s+|an?\s+)?(?:\w+\s+)?$", before):
        return Item("ore", start, m.end(), {"ore": ORES[word], "count": fields["count"] or 4})
    return Item("item", start, m.end(), fields)


def _version(s: Sentence) -> Optional[str]:
    m = re.search(r"\b(?:in |on |for |version |mc |minecraft |java |v)*\b(1\.\d{1,2}(?:\.\d{1,2})?|2\d\.\d{1,2}(?:\.\d{1,2})?)\b", s.text)
    if not m:
        return None
    s.use(m.start(), m.end())
    return m.group(1)


RADIUS = re.compile(r"(?:(?:within|in|under|less than|at most|up to|no more than|about|around|<)\s+)?"
                    r"(\d+)\s*(blocks?|chunks?|b\b|m\b)?")
RELATE = re.compile(r"\b(?:right\s+|directly\s+|just\s+|very\s+)?"
                    r"(next to|next door to|adjacent to|beside|touching|by|near|close to|"
                    r"around|from|of|in|inside|on|under|underneath|below|above)"
                    r"\s*(?:an?\s+|the\s+|some\s+)?$")
REF_WORDS = re.compile(r"\b(?:the\s+)?(spawn|spawn point|origin|world origin|center|centre|"
                       r"middle(?: of the world)?|0,\s*0)\b")
NEARBY_TAIL = re.compile(r"\b(nearby|close by|close|near(?:by)? (?:it|there)|next to it|by it|"
                         r"around it|beside it)\b")


def _read_gap(s: Sentence, start, end, item: Item, nxt: Optional[Item]):
    """What the words between two items say about the one on the left."""
    gap = s.text[start:end]

    m = None
    for candidate in RADIUS.finditer(gap):
        unit = candidate.group(2)
        had_word = candidate.group(0)[:1].isalpha()
        if unit or had_word:
            m = candidate
            break
    if m:
        item.within = int(m.group(1))
        item.chunks = bool(m.group(2) and m.group(2).startswith("chunk"))
        s.use(start + m.start(), start + m.end())

    r = REF_WORDS.search(gap)
    if r:
        word = r.group(1)
        item.of = "spawn" if word.startswith("spawn") else "origin"
        s.use(start + r.start(), start + r.end())
        if not m:
            item.within = 96 if re.search(r"\b(at|on|right)\b", gap[:r.start()]) else None

    t = NEARBY_TAIL.search(gap)
    if t and not r:
        item.near_prev = True
        s.use(start + t.start(), start + t.end())

    if nxt is not None and not r:
        rel = RELATE.search(gap)
        if rel:
            word = rel.group(1)
            s.use(start + rel.start(), end)
            if word in ("in", "inside", "on", "under", "underneath", "below", "above"):
                if item.within is None:
                    item.within = INSIDE
            elif word in ("next to", "next door to", "adjacent to", "beside", "touching", "by"):
                if item.within is None:
                    item.within = ADJACENT
            elif item.within is None:
                item.within = NEAR
            item.parent = nxt


def _assign_ids(items: List[Item]):
    counts = {}
    for it in items:
        base = (it.fields.get("structure") or it.fields.get("biome") or it.fields.get("ore")
                or it.kind)[:12]
        counts[base] = counts.get(base, 0) + 1
        it.id = base if counts[base] == 1 else f"{base}{counts[base]}"


def _condition(it: Item, version_fields: dict) -> dict:
    c = {"id": it.id}
    c.update({k: v for k, v in it.fields.items() if k not in ("word",)})
    if it.negate:
        c["not"] = True
    if it.kind in ("eyes",):
        return c
    if it.kind == "island":
        c["of"] = "spawn"
        c["pct"], c["within"] = 75, it.within or 256
        return c
    within = it.within if it.within is not None else DEFAULT_WITHIN.get(it.kind, 1000)
    if it.chunks:
        within *= 16
        c["metric"] = "chebyshev"
    c["within"] = within
    c["of"] = it.parent.id if it.parent else (it.of or "origin")
    return c


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
    loot = (loot or {}).get("structures", {})

    # "or" between two items puts them in one group.
    group = 0
    for a, b in zip(items, items[1:]):
        if re.fullmatch(r"\s*(?:,\s*)?(?:or|either)\s+(?:an?\s+|the\s+|some\s+)?", s.text[a.end:b.start]):
            if a.group is None:
                group += 1
                a.group = group
            b.group = a.group
            s.use(a.end, b.start)

    # "no ocean", "without a village".
    for it in items:
        before = s.text[:it.start]
        m = re.search(r"\b(?:no|not|without|never|avoid|but no|and no)\s+(?:an?\s+|any\s+|the\s+)?$", before)
        if m and it.kind in ("structure", "biome"):
            it.negate = True
            s.use(m.start(), it.start)

    # A chest item belongs to the structure before it.
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
    # "a stronghold with 8 eyes": the eye count already is that stronghold.
    if any(it.kind == "eyes" for it in kept):
        kept = [it for it in kept if it.kind != "stronghold" or it.fields["stronghold"] is not True]
    items = kept

    # Distances and references.
    for i, it in enumerate(items):
        nxt = items[i + 1] if i + 1 < len(items) else None
        end = nxt.start if nxt else len(s.text)
        _read_gap(s, it.end, end, it, nxt)
    if items:
        _read_gap(s, 0, items[0].start, items[0], None)

    for i, it in enumerate(items):
        if it.near_prev and i and it.parent is None:
            it.parent = items[i - 1]
            if it.within is None:
                it.within = NEAR
        if it.parent is None and i and it.kind == "biome" and items[i - 1].kind in ("structure", "loot") \
                and re.search(r"\bwith\b", s.text[items[i - 1].end:it.start]) and not it.of:
            it.parent, it.within = items[i - 1], it.within or NEAR

    # A biome cannot anchor a structure: "a mansion in a jungle" is a jungle
    # around the mansion. Turn the link around, keeping the distance.
    for it in items:
        p = it.parent
        if p is not None and p.kind == "biome" and it.kind in ("structure", "loot", "stronghold"):
            p.parent, p.within, it.parent = it, it.within, None
            it.within = None

    # "a mansion and a village near spawn": a place named once covers the
    # conditions that named none.
    place = next((it for it in reversed(items) if it.of), None)
    if place:
        for it in items:
            if it.parent is None and it.of is None:
                it.of = place.of
                if it.within is None and place.within is not None and it.kind == place.kind:
                    it.within = place.within

    _assign_ids(items)
    groups, conditions = {}, []
    for it in items:
        c = _condition(it, {})
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
    return {"query": query, "notes": "; ".join(notes), "ignored": refused + s.leftovers()}


def want_version(text: str) -> Optional[str]:
    """The version a sentence names, if any: the vocabulary has to be that one's."""
    s = Sentence(_normalise(text))
    return _version(s)
