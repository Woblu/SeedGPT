#!/usr/bin/env python3
"""The offline prompt: what people type, and what it must become.

    python tests/test_nlparse.py

Every parsed query also goes through the real engine (`find <query> 1 1`), so
this fails if the parser ever writes a query the engine refuses.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
import nlparse  # noqa: E402

VERSION = "1.21"


def binary(name):
    for candidate in (ROOT / "build" / f"{name}.exe", ROOT / "build" / name):
        if candidate.exists():
            return str(candidate)
    raise SystemExit(f"build/{name}.exe is missing. Run: ./build.sh all")


def tool_json(name, version=VERSION):
    return json.loads(subprocess.run([binary(name), version], capture_output=True, text=True, check=True).stdout)


VOCAB, LOOT = tool_json("vocab"), tool_json("lootitems")


def ask(text):
    r = nlparse.parse(text, VERSION, VOCAB, LOOT)
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as f:
        json.dump(r["query"], f)
    try:
        done = subprocess.run([binary("find"), f.name, "1", "1"], capture_output=True, text=True)
    finally:
        os.unlink(f.name)
    assert done.returncode in (0, 3), f"{text!r}: engine refused {r['query']}: {done.stderr}"
    return r


def conds(text):
    return ask(text)["query"]["conditions"]


class Structures(unittest.TestCase):
    def test_distance_from_spawn(self):
        c = conds("a woodland mansion within 300 blocks of spawn")
        self.assertEqual([(c[0]["structure"], c[0]["within"], c[0]["of"])], [("mansion", 300, "spawn")])

    def test_no_place_named_means_the_middle_of_the_world(self):
        c = conds("a village")
        self.assertEqual((c[0]["of"], c[0]["within"]), ("origin", 2000))

    def test_spoken_names(self):
        for said, engine in (("witch hut", "swamp_hut"), ("pillager outpost", "outpost"),
                             ("desert temple", "desert_pyramid"), ("ocean monument", "monument"),
                             ("nether fortress", "fortress"), ("ancient city", "ancient_city")):
            self.assertEqual(conds(f"a {said}")[0]["structure"], engine, said)

    def test_jungle_temple_is_not_a_jungle(self):
        c = conds("a jungle temple")
        self.assertEqual(len(c), 1)
        self.assertEqual(c[0]["structure"], "jungle_temple")

    def test_variants(self):
        self.assertTrue(conds("a zombie village")[0]["abandoned"])
        self.assertTrue(conds("an igloo with a basement")[0]["basement"])
        self.assertTrue(conds("an end ship within 3000 blocks")[0]["ship"])

    def test_chunks_are_a_square(self):
        c = conds("a village within 5 chunks of spawn")
        self.assertEqual((c[0]["within"], c[0]["metric"]), (80, "chebyshev"))

    def test_one_place_covers_both(self):
        c = conds("a mansion and a village within 1,000 blocks of spawn")
        self.assertEqual({(x["structure"], x["of"], x["within"]) for x in c},
                         {("mansion", "spawn", 1000), ("village", "spawn", 1000)})


class Relations(unittest.TestCase):
    def test_right_next_to(self):
        c = conds("village right next to a mansion")
        by = {x.get("structure"): x for x in c}
        self.assertEqual(by["village"]["of"], by["mansion"]["id"])
        self.assertEqual(by["village"]["within"], 200)

    def test_explicit_distance_between_two(self):
        c = conds("a village within 150 blocks of a mansion")
        by = {x.get("structure"): x for x in c}
        self.assertEqual((by["village"]["of"], by["village"]["within"]), (by["mansion"]["id"], 150))

    def test_nearby_points_back(self):
        c = conds("a mansion with a village nearby")
        by = {x.get("structure"): x for x in c}
        self.assertEqual(by["village"]["of"], by["mansion"]["id"])

    def test_structure_in_a_biome_turns_the_link_around(self):
        c = conds("a mansion in a cherry grove")
        by = {("biome" if "biome" in x else "structure"): x for x in c}
        self.assertEqual(by["biome"]["of"], by["structure"]["id"])
        self.assertEqual(by["biome"]["within"], 64)


class Biomes(unittest.TestCase):
    def test_landscape_means_the_family(self):
        c = conds("a badlands within 500 blocks of spawn")
        self.assertTrue(c[0]["family"])

    def test_exact_biome_stays_exact(self):
        self.assertNotIn("family", conds("a cherry grove")[0])

    def test_mushroom_island_is_the_biome_not_an_island_spawn(self):
        c = conds("a mushroom island")
        self.assertEqual(c[0]["biome"], "mushroom_fields")

    def test_absence(self):
        c = conds("no ocean within 300 blocks of spawn")
        self.assertTrue(c[0]["not"])
        self.assertEqual((c[0]["biome"], c[0]["within"]), ("ocean", 300))


class OresAndLoot(unittest.TestCase):
    def test_ore(self):
        c = conds("diamonds within 64 blocks of spawn")
        self.assertEqual((c[0]["ore"], c[0]["within"], c[0]["of"]), ("diamond", 64, "spawn"))

    def test_ore_count(self):
        self.assertEqual(conds("12 diamonds within 48 blocks")[0]["count"], 12)

    def test_chest_loot_follows_a_structure(self):
        c = conds("a desert pyramid with 2 diamonds")
        self.assertEqual(c[0]["loot"], {"structure": "desert_pyramid", "item": "diamond", "count": 2})

    def test_named_item(self):
        r = ask("a desert pyramid with an enchanted golden apple")
        self.assertEqual(r["ignored"], [])
        self.assertEqual(r["query"]["conditions"][0]["loot"]["item"], "enchanted_golden_apple")

    def test_an_item_that_chest_cannot_hold_is_not_dropped_quietly(self):
        r = ask("a shipwreck with an enchanted golden apple")
        self.assertIn("enchanted golden apple", r["ignored"])
        self.assertIn("do not hold", r["notes"])


class Specials(unittest.TestCase):
    def test_eyes(self):
        self.assertEqual(conds("an 8 eye portal")[0]["eyes"], 8)

    def test_stronghold_with_eyes_is_one_condition(self):
        r = ask("a stronghold with 8 eyes in the portal")
        self.assertEqual([x["eyes"] for x in r["query"]["conditions"]], [8])
        self.assertEqual(r["ignored"], [])

    def test_island(self):
        c = conds("a survival island")
        self.assertEqual((c[0]["island"], c[0]["of"]), (True, "spawn"))

    def test_slime(self):
        self.assertEqual(conds("6 slime chunks within 64 blocks")[0]["slime"], 6)


class AnyOf(unittest.TestCase):
    def test_or_is_a_group(self):
        c = conds("a village or a pillager outpost within 500 blocks of spawn")
        self.assertEqual(len(c), 1)
        self.assertEqual({x["structure"] for x in c[0]["any"]}, {"village", "outpost"})

    def test_or_alongside_and(self):
        c = conds("a mansion or a monument, and a village within 800 blocks")
        groups = [x for x in c if "any" in x]
        self.assertEqual(len(groups), 1)
        self.assertEqual(len(c), 2)


class Honesty(unittest.TestCase):
    def test_a_clause_it_cannot_read_is_reported(self):
        r = ask("a village with at least 5 smiths")
        self.assertIn("smiths", r["ignored"])

    def test_a_sentence_with_nothing_in_it_fails(self):
        with self.assertRaises(nlparse.NotUnderstood):
            nlparse.parse("something really cool please", VERSION, VOCAB, LOOT)

    def test_clean_requests_ignore_nothing(self):
        for text in ("i want a woodland mansion within 300 blocks of spawn",
                     "find me a seed with a village near spawn",
                     "diamonds within 64 blocks of spawn"):
            self.assertEqual(ask(text)["ignored"], [], text)

    def test_version_in_the_sentence(self):
        self.assertEqual(nlparse.want_version("a village in 1.16"), "1.16")
        self.assertEqual(nlparse.want_version("a village on minecraft 26.2"), "26.2")
        self.assertIsNone(nlparse.want_version("a village within 300 blocks"))

    def test_a_version_without_the_structure_cannot_ask_for_it(self):
        old = tool_json("vocab", "1.12")
        with self.assertRaises(nlparse.NotUnderstood):
            nlparse.parse("an ancient city", "1.12", old)


class NeverSilentlyWrong(unittest.TestCase):
    """Each of these once came back with nothing ignored and a query for something
    other than what was asked, which is the one thing this reader must not do."""

    def asks_first(self, text):
        r = ask(text)
        self.assertTrue(r["ignored"], f"{text!r} ran silently as {r['query']['conditions']}")
        return r

    def test_a_version_is_not_a_distance(self):
        self.assertEqual(conds("a village in 1.20")[0]["within"], 2000)
        self.assertEqual(nlparse.want_version("a village within 1.5 chunks"), None)
        self.assertEqual(conds("a village within 1.5 chunks")[0]["within"], 24)

    def test_a_minimum_distance_is_not_a_maximum(self):
        self.asks_first("a village at least 1000 blocks from spawn")
        self.asks_first("a village more than 500 blocks from spawn")
        self.asks_first("a village away from an ocean")

    def test_without_is_not_with(self):
        self.asks_first("a village without diamonds")
        self.asks_first("a mansion without a stronghold")
        self.asks_first("no slime chunks")

    def test_neither_means_both_absent(self):
        c = conds("no ocean or river within 300 blocks of spawn")
        self.assertEqual(len(c), 2)
        self.assertTrue(all(x["not"] and x["within"] == 300 and x["of"] == "spawn" for x in c))

    def test_chunks_survive_sharing_and_turning_around(self):
        c = conds("a mansion and a village within 5 chunks of spawn")
        self.assertEqual({(x["within"], x["metric"]) for x in c}, {(80, "chebyshev")})
        by = {("biome" if "biome" in x else "structure"): x for x in conds("a mansion within 5 chunks of a jungle")}
        self.assertEqual((by["biome"]["within"], by["biome"]["metric"]), (80, "chebyshev"))
        self.assertEqual(by["structure"]["within"], 2000)

    def test_a_list_with_or_is_all_alternatives(self):
        c = conds("a village, a mansion, or a monument")
        self.assertEqual(len(c), 1)
        self.assertEqual(len(c[0]["any"]), 3)

    def test_loot_keeps_its_place_in_a_group(self):
        c = conds("a desert pyramid with 2 diamonds or a buried treasure")
        self.assertEqual(len(c), 1)
        self.assertEqual(len(c[0]["any"]), 2)

    def test_a_link_into_a_group_is_not_guessed(self):
        self.asks_first("a mansion in a jungle or a desert")
        self.asks_first("a village near a mansion or a monument")
        self.asks_first("a village or a monument near a mansion")

    def test_a_biome_between_two_structures_keeps_both_links(self):
        by = {x.get("structure") or "biome": x for x in conds("a village in a jungle near a mansion")}
        self.assertEqual(by["biome"]["of"], by["village"]["id"])
        self.assertEqual(by["village"]["of"], by["mansion"]["id"])

    def test_a_stronghold_distance_is_not_lost_to_its_eyes(self):
        self.asks_first("a stronghold within 1000 blocks of spawn with 8 eyes")

    def test_a_place_and_a_link_both_count(self):
        by = {x["structure"]: x for x in conds("a village within 500 blocks of spawn near a mansion")}
        self.assertEqual((by["village"]["of"], by["village"]["within"]), ("spawn", 500))
        self.assertEqual(by["mansion"]["of"], by["village"]["id"])

    def test_how_close_to_spawn(self):
        self.assertEqual(conds("a village near spawn")[0]["within"], 400)
        self.assertEqual(conds("a village next to spawn")[0]["within"], 200)
        self.assertEqual(conds("a village right at spawn")[0]["within"], 96)
        self.assertEqual(conds("i want to spawn in a village")[0]["of"], "spawn")

    def test_every_ore_name_is_read(self):
        self.assertEqual(conds("copper within 64 blocks")[0]["ore"], "copper")
        self.assertEqual(conds("ancient debris within 64 blocks")[0]["ore"], "ancient_debris")

    def test_a_count_is_not_a_stray_word(self):
        self.assertEqual(ask("12 diamonds within 48 blocks")["ignored"], [])
        self.asks_first("less than 5 diamonds within 48 blocks")

    def test_the_nether_is_not_measured_from_spawn(self):
        r = ask("a fortress or a village within 1000 blocks of spawn")
        members = {x["structure"]: x for x in r["query"]["conditions"][0]["any"]}
        self.assertEqual((members["fortress"]["of"], members["village"]["of"]), ("origin", "spawn"))
        self.assertIn("nether", r["notes"])

    def test_words_that_change_meaning_are_never_filler(self):
        for word in ("without", "near", "under", "more", "less", "than", "least", "away", "not", "no"):
            self.assertNotIn(word, nlparse.FILLER)


if __name__ == "__main__":
    unittest.main(verbosity=1)
