# Advanced tools

The main search is fast because it never builds the world, which means it cannot see
anything decided while a world generates. A few questions need exactly that. These
tools answer them by running Minecraft's own generator, or a real server.

They are not in the download, because they depend on Minecraft's game files, which may
not be redistributed, and they are developer tools rather than finished features. They
have only been run on Windows. Once set up, they appear in the **Tools** tab on their own.

| Tool | Answers | Needs |
|---|---|---|
| Village buildings | Seeds whose village has N smiths | 1.16.5 backend |
| Exposed treasure | A buried treasure chest in open air | 1.21.1 backend |
| Cave spiders by a village | Villages next to spider spawners | 1.21.1 backend |
| Actually floating? | Whether a structure is really in the air | 1.21.1 backend |
| Treasure casing | A chest walled in by magma, ore or gravel | 1.21.1 backend and a server |
| Bedrock glitched outpost | The outpost on a tall cobblestone pillar | Bedrock Dedicated Server |

Fortress overlap, structure overlap, "what is cool in my seed?" and "keep looking" are
already in the download.

## Before you start

Run Seedsmith from source (see the [README](../README.md#build-from-source)). You also
need [Gradle 8](https://gradle.org/install/), JDK 17 for the 1.16.5 backend, and JDK 21
for the 1.21.1 backend.

These steps download Minecraft from Mojang for use on your own computer, so
[Mojang's EULA](https://www.minecraft.net/eula) applies. Never commit or share those files.

## 1.16.5 backend

From `tier2/`, with JDK 17:

```sh
gradle --no-daemon printcp -q | sed 's/^CP://' | tr '\n' ';' > cp.txt
javac -cp "$(cat cp.txt)" -d out src/main/java/VillageWorldgen.java
```

[`tier2/README.md`](../tier2/README.md) explains how it works.

## 1.21.1 backend

From `tier2-outpost/`, with JDK 21:

```sh
gradle --no-daemon printcp -q
javac -cp "$(cat cp.txt)" -d out src/main/java/*.java
```

## A real server

Treasure casing confirms every hit on a real server. Fetch `tier3-java/server.jar` with
the steps in [`tier3-java/README.md`](../tier3-java/README.md#setup), which check the
download against Mojang's published hash.

## Bedrock

Download the Bedrock Dedicated Server from Minecraft's website and unpack it into
`tier3-bedrock/server/`.
