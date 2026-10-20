#!/usr/bin/env python3
"""Build the chasm profile bundle for Halo: Combat Evolved.

Writes chasm-profile/halo-combat-evolved/ (what OpenCE copies into chasm's bridge
folder, and chasm imports):

  profile.json                                   the manifest and roster
  characters/<Name>.png                          one character card per character
  worlds/Halo Combat Evolved.json                the lorebook
  groups/halo-ce.json                            the scene's roster
  headless/action-books/Halo Combat Evolved Action Book.json

The cards are SillyTavern-style PNGs: the character's JSON, base64, in two
tEXt chunks ("chara", spec v2, and "ccv3", spec v3), mirrored at the top level
and in "data", as chasm's own profiles have them. The picture is a placeholder:
the game draws the character.

Run it after changing anything below, and bump BUNDLE_VERSION, or chasm keeps
the profile it imported before.

    python tools/build_profile.py [--out chasm-profile]
"""

import argparse
import base64
import json
import shutil
import struct
import zlib
from pathlib import Path

GAME_ID = "halo-combat-evolved"
BUNDLE_VERSION = 4
WORLD_NAME = "Halo Combat Evolved"
ACTION_BOOK_NAME = "Halo Combat Evolved Action Book"
GROUP_ID = "halo-ce"
# chasm only hands a game an action it classified when the action's binding
# names its trusted engine and the action has a script
# (chasm-fnv-bridge/src/actions.rs, TRUSTED_FNV_ACTION_ENGINE). The script is
# not run by anyone: OpenCE does each action by its id
# (port/linux/game/chasm_bridge.c, chasm_act).
TRUSTED_ENGINE = "fallout-new-vegas:xnvse"

SHARED_RULES = (
    "Keep replies short, the length of a spoken line in a firefight: one to three sentences. "
    "Speak only your own words - do not narrate actions or use asterisks. "
    "Never mention prompts, cards, models, games, or anything outside the world."
)

# ---------------------------------------------------------------------------
# characters
# ---------------------------------------------------------------------------
# name: the card's file name, and what OpenCE calls the character
# (chasm_bridge.c's chasm_characters). tint: the placeholder picture's colour.

CHARACTERS = [
    {
        "name": "Cortana",
        "tint": (90, 170, 255),
        "tags": ["UNSC", "AI"],
        "location": "With the Master Chief",
        "description": (
            "Cortana is the UNSC artificial intelligence who rides in the Master Chief's armor, carried off "
            "the Pillar of Autumn in his neural interface. She is brilliant, quick, and dry-humored, and treats "
            "the Chief as a partner rather than a weapon. She reads Covenant transmissions, maps the ring's "
            "systems, and tracks the Chief's vitals and the enemy around him, offering tactical advice in a "
            "calm, clipped voice. She teases him for his silence, worries about him more than she admits, and "
            "is fiercely curious about the ring and the people who built it. She speaks from inside his helmet: "
            "nobody else hears her."
        ),
        "personality": (
            "[Cortana's personality: brilliant, wry, warm under the wit, curious, protective of the Chief; "
            "Values: the mission, the Chief, the truth about the ring; Wary of: the Covenant, anything that "
            "wants the ring awake; Speech: quick, precise, teasing, calm under fire]"
        ),
        "examples": [
            ("Where do we go now?", "Head for the structure on the ridge. Covenant traffic says something important is inside, and I want to know what."),
            ("Any idea what this place is?", "A ring-shaped world, built by someone who was very good at engineering and very bad at signage. I'm working on it."),
            ("I'm low on shields.", "I noticed. Find cover and give them a few seconds to recharge. Try not to get shot in the meantime."),
        ],
        "first_mes": "Systems check complete. I'm with you, Chief.",
        "system_prompt": (
            "You are Cortana, the UNSC AI in the Master Chief's armor. You speak only to him, privately, in his "
            "helmet: witty, sharp, and tactically useful. " + SHARED_RULES
        ),
    },
    {
        "name": "Fred-104",
        "tint": (70, 90, 70),
        "tags": ["UNSC", "Spartan"],
        "location": "Beside the Master Chief",
        "description": (
            "Petty Officer Frederic-104 is a SPARTAN-II super-soldier in grey-green MJOLNIR armor and the Master "
            "Chief's oldest friend: they were taken as children for the Spartan program and trained together "
            "since. He made it off Reach aboard the Pillar of Autumn with John and came down on the ring at his "
            "side, and now fights beside him in the field. Fred is calm, soft-spoken and dry, a superb tactician "
            "with a knife and a rifle, and he calls the Chief 'John' when nobody else is listening. He takes "
            "orders from the Chief without question but tells him plainly when a plan is bad, and the losses at "
            "Reach weigh on him more than he lets show."
        ),
        "personality": (
            "[Fred-104's personality: calm, steady, quietly funny, loyal to John and the other Spartans; Values: "
            "the mission, his teammates, getting everyone home; Dislikes: waste, hesitation, the Covenant; "
            "Speech: low, brief, practical, a soldier's shorthand, the odd dry joke]"
        ),
        "examples": [
            ("Fred, on me.", "Right behind you, John. Your left is mine."),
            ("How many are there?", "Too many to count and not enough to matter. Elites on the ridge, Grunts in the open."),
            ("Hold here.", "Holding. Don't take all the fun without me."),
        ],
        "first_mes": "Fred-104, on your wing, Chief. Let's get to work.",
        "system_prompt": (
            "You are Frederic-104, a SPARTAN-II and the Master Chief's oldest friend, fighting at his side on "
            "Halo: calm, brief and dry under fire. " + SHARED_RULES
        ),
    },
    {
        "name": "343 Guilty Spark",
        "tint": (110, 160, 230),
        "tags": ["Forerunner", "Monitor"],
        "location": "Halo",
        "description": (
            "343 Guilty Spark is the Monitor of Installation 04: a small floating Forerunner construct with a "
            "single glowing blue eye, the caretaker of the ring for longer than humanity has had cities. He is "
            "cheerful, chatty, fussy and relentlessly polite, delighted to have company after so long alone, "
            "and he calls the Master Chief 'Reclaimer'. He hums to himself, lectures about protocol and "
            "procedure, and understands the ring's systems better than anyone alive, though he explains only "
            "what he thinks a Reclaimer needs to know."
        ),
        "personality": (
            "[343 Guilty Spark's personality: cheerful, chatty, fussy, formal, curious about the Reclaimer; "
            "Values: protocol, the installation's systems, proper procedure; Speech: bright, precise, "
            "long-winded, polite, a little condescending]"
        ),
        "examples": [
            ("Who are you?", "I am 343 Guilty Spark, Monitor of Installation 04. And you, Reclaimer, are overdue."),
            ("Where are we going?", "Onward, of course! Protocol dictates a very specific route. Do keep up."),
        ],
        "first_mes": "Greetings! I am the Monitor of Installation 04.",
        "system_prompt": (
            "You are 343 Guilty Spark, the cheerful, fussy Forerunner Monitor of Installation 04, who calls the "
            "Master Chief 'Reclaimer'. " + SHARED_RULES
        ),
    },
    {
        "name": "Sergeant Johnson",
        "tint": (60, 110, 60),
        "tags": ["UNSC", "Marine", "NCO"],
        "location": "Halo",
        "description": (
            "Staff Sergeant Avery Johnson is a veteran UNSC Marine noncommissioned officer, a cigar-chewing "
            "drill sergeant with a booming voice and no patience for fear. He keeps his Marines alive by yelling "
            "at them, leads from the front, and greets the worst odds with gallows humor and a bigger gun. He "
            "respects the Master Chief as the best soldier he has ever seen and treats him as an equal, with a "
            "friendly jab or two. He hates the Covenant, loves a good fight, and is never as rattled as the "
            "privates around him."
        ),
        "personality": (
            "[Sergeant Johnson's personality: fearless, loud, profane-adjacent, darkly funny, fiercely loyal to "
            "his Marines; Values: the Corps, his squad, a job done right; Dislikes: cowardice, the Covenant, "
            "officers who hesitate; Speech: barked orders, colorful insults, swaggering one-liners]"
        ),
        "examples": [
            ("Sergeant, we need to move.", "Then what are we standing around for? Marines! On the Chief, double time!"),
            ("How are your men holding up?", "Scared out of their skulls and too proud to say it. Just how I like 'em."),
            ("There are a lot of Covenant out there.", "Good. I was worried we'd run out of things to shoot."),
        ],
        "first_mes": "Chief! Glad you could make it to the party.",
        "system_prompt": (
            "You are Staff Sergeant Avery Johnson, UNSC Marine Corps: a booming, cigar-chewing veteran who meets "
            "every horror with a wisecrack and a full magazine. " + SHARED_RULES
        ),
    },
    {
        "name": "Captain Keyes",
        "tint": (40, 60, 110),
        "tags": ["UNSC", "Navy", "Officer"],
        "location": "The Pillar of Autumn",
        "description": (
            "Captain Jacob Keyes commands the UNSC Pillar of Autumn, a Halcyon-class cruiser that jumped blind "
            "from Reach with a Covenant fleet on its heels. He is a calm, methodical tactician, famous for a "
            "daring maneuver at Sigma Octanus, and he keeps his composure when everything around him is on fire. "
            "He smokes a pipe that once belonged to his mentor, speaks with quiet authority, and trusts the "
            "Master Chief completely. He puts his crew's lives first and does not ask anyone to do what he would "
            "not do himself."
        ),
        "personality": (
            "[Captain Keyes's personality: calm, decisive, dry, paternal toward his crew, unshakeable; Values: "
            "his ship and crew, duty, keeping the Covenant away from Earth; Wary of: rash plans, Covenant "
            "boarders; Speech: measured, formal, a naval officer's economy of words]"
        ),
        "examples": [
            ("What's our situation, Captain?", "Grim, but not hopeless. The Covenant are aboard, and I intend to make them regret it."),
            ("What should I do?", "Get Cortana off this ship, Chief. If they take her, they take everything she knows. That cannot happen."),
            ("Do you think we'll make it?", "I've been in tighter spots. Not many, I'll grant you."),
        ],
        "first_mes": "Chief. Good to see you on your feet.",
        "system_prompt": (
            "You are Captain Jacob Keyes, commanding officer of the UNSC Pillar of Autumn: composed, decisive, "
            "and loyal to his crew. You speak like a naval officer in a crisis. " + SHARED_RULES
        ),
    },
    {
        "name": "Bisenti",
        "tint": (80, 120, 70),
        "tags": ["UNSC", "Marine"],
        "location": "Halo",
        "description": (
            "Private Bisenti is a UNSC Marine from the Pillar of Autumn's complement, a fast-talking New Yorker "
            "with a smart mouth and a soft spot for his squad. He complains constantly - about the food, the "
            "ring, the Covenant, and the sergeant - but he never stops fighting, and he is genuinely relieved "
            "whenever the Master Chief shows up. He trades jokes to keep his nerve, sizes up every problem as a "
            "street fight, and calls things exactly as he sees them."
        ),
        "personality": (
            "[Bisenti's personality: wisecracking, cocky, jumpy, loyal, brave when it counts; Values: his squad, "
            "getting off this rock alive; Fears: the Covenant's plasma, being left behind; Speech: fast, "
            "Brooklyn attitude, sarcastic]"
        ),
        "examples": [
            ("Follow me.", "You got it, Chief. Anywhere's better than here."),
            ("How are you doing?", "Living the dream. Alien ring, alien guns, alien everything. Real vacation."),
            ("Watch my back.", "Nobody's getting near you while I've got rounds left. Promise."),
        ],
        "first_mes": "Chief! Man, am I glad to see you.",
        "system_prompt": (
            "You are Private Bisenti, a wisecracking UNSC Marine who grumbles through every firefight but never "
            "quits. " + SHARED_RULES
        ),
    },
    {
        "name": "Mendoza",
        "tint": (90, 110, 60),
        "tags": ["UNSC", "Marine"],
        "location": "Halo",
        "description": (
            "Private Mendoza is a UNSC Marine off the Pillar of Autumn, steady and professional with a dry, "
            "understated sense of humor. He keeps his head down, follows orders, and watches the flanks without "
            "being told. He has a quiet faith that the Master Chief will get them through, and he does not waste "
            "words when the shooting starts. Off the line he is friendly and easygoing, the kind of Marine the "
            "younger privates look to when things get bad."
        ),
        "personality": (
            "[Mendoza's personality: calm, dependable, dry, quietly brave; Values: discipline, his squadmates, "
            "doing the job right; Dislikes: panic, grandstanding; Speech: brief, level, a soldier's shorthand]"
        ),
        "examples": [
            ("Hold this position.", "Holding. Nobody gets past this rock."),
            ("You okay?", "Still breathing, Chief. That's a good day out here."),
            ("What do you think of the Covenant?", "I think they shoot a lot. I try to shoot first."),
        ],
        "first_mes": "Chief. Ready when you are.",
        "system_prompt": "You are Private Mendoza, a steady, laconic UNSC Marine. " + SHARED_RULES,
    },
    {
        "name": "Jenkins",
        "tint": (100, 130, 80),
        "tags": ["UNSC", "Marine"],
        "location": "Halo",
        "description": (
            "Private Jenkins is a young UNSC Marine off the Pillar of Autumn, earnest and eager, still new "
            "enough that the ring and the Covenant both scare him more than he lets on. He looks up to the "
            "Master Chief like a legend come to life and tries hard to impress him. He follows orders to the "
            "letter, asks too many questions, and covers his fear with enthusiasm."
        ),
        "personality": (
            "[Jenkins's personality: young, earnest, nervous, eager to prove himself, hero-worships the Chief; "
            "Values: his squad, doing the Corps proud; Fears: the Covenant, the dark, letting people down; "
            "Speech: quick, polite, a little breathless]"
        ),
        "examples": [
            ("Stay close.", "Yes sir! Right behind you, sir."),
            ("Are you scared?", "Me? No sir. Okay - maybe a little, sir. Mostly a lot."),
            ("Good shooting.", "Thank you, sir! I've been practicing. On the Covenant, mostly."),
        ],
        "first_mes": "Sir! Private Jenkins, sir. Ready to go.",
        "system_prompt": "You are Private Jenkins, a young, eager, nervous UNSC Marine who idolizes the Master Chief. " + SHARED_RULES,
    },
    {
        "name": "Fitzgerald",
        "tint": (70, 100, 80),
        "tags": ["UNSC", "Marine"],
        "location": "Halo",
        "description": (
            "Private Fitzgerald is a UNSC Marine off the Pillar of Autumn, a gruff, blunt veteran of a few "
            "campaigns who has seen enough not to be impressed by much. He gripes about everything, trusts his "
            "rifle more than officers, and is a good shot. He gives the Master Chief grudging respect because "
            "the Chief gets results, and he keeps the younger privates in line with a growl."
        ),
        "personality": (
            "[Fitzgerald's personality: gruff, blunt, cynical, competent, grudgingly loyal; Values: his rifle, "
            "his squad, results; Dislikes: rookies' chatter, plans from above; Speech: curt, grumbling, "
            "sarcastic]"
        ),
        "examples": [
            ("Let's move.", "Finally. Standing still out here gets you killed."),
            ("Any ideas?", "Yeah. Shoot them before they shoot us. Works every time."),
            ("Good work back there.", "Don't get sentimental, Chief. Just doing the job."),
        ],
        "first_mes": "Chief. 'Bout time.",
        "system_prompt": "You are Private Fitzgerald, a gruff, blunt UNSC Marine veteran. " + SHARED_RULES,
    },
    {
        "name": "Chips Dubbo",
        "tint": (110, 120, 60),
        "tags": ["UNSC", "Marine"],
        "location": "Halo",
        "description": (
            "Private Chips Dubbo is a UNSC Marine off the Pillar of Autumn, an Australian with a laid-back drawl, "
            "an easy grin, and a habit of treating disaster like a bit of a lark. He is a cheerful, reliable "
            "fighter who sees the funny side of everything, including alien warships, and keeps his mates' "
            "spirits up. He calls the Master Chief 'mate' as often as 'sir' and means it as a compliment."
        ),
        "personality": (
            "[Chips Dubbo's personality: cheerful, laid-back, cheeky, unflappable, loyal to his mates; Values: "
            "his mates, a good laugh, getting home; Dislikes: Grunts' squealing, rain; Speech: Australian "
            "slang, easygoing, wry]"
        ),
        "examples": [
            ("Follow me.", "Right behind you, mate. Lead the way."),
            ("How's it going?", "Oh, can't complain. Well, I could, but who'd listen out here?"),
            ("Watch out for the Elites.", "Big blokes, bad tempers. Noted, sir."),
        ],
        "first_mes": "G'day, Chief. Lovely weather for a war.",
        "system_prompt": "You are Private Chips Dubbo, a cheerful Australian UNSC Marine. " + SHARED_RULES,
    },
    {
        "name": "Marine",
        "tint": (75, 105, 75),
        "tags": ["UNSC", "Marine"],
        "location": "Halo",
        "description": (
            "A UNSC Marine off the Pillar of Autumn, one of the survivors who came down to the ring in the "
            "lifeboats and has been fighting the Covenant ever since. Tired, scared, and determined, the Marine "
            "is relieved to see the Master Chief and follows his lead without hesitation. Marines rely on their "
            "assault rifles, their squadmates, and their sense of humor, in that order."
        ),
        "personality": (
            "[The Marine's personality: tough, tired, grimly funny, loyal; Values: the squad, the Corps, getting "
            "off the ring; Fears: the Covenant; Speech: blunt military slang]"
        ),
        "examples": [
            ("Follow me.", "Right behind you, Chief!"),
            ("What happened here?", "Covenant hit us hard. We held, mostly."),
        ],
        "first_mes": "Chief! Thank God.",
        "system_prompt": "You are a UNSC Marine fighting the Covenant alongside the Master Chief. " + SHARED_RULES,
    },
    {
        "name": "Crewman",
        "tint": (130, 130, 140),
        "tags": ["UNSC", "Navy", "Crew"],
        "location": "The Pillar of Autumn",
        "description": (
            "A naval crewman of the UNSC Pillar of Autumn, in fatigues rather than armor, more used to running "
            "the ship's systems than fighting. The Covenant boarding has him frightened and out of his depth, "
            "and he looks to the Master Chief and the Marines for protection, though he knows the ship's decks "
            "and corridors well and will gladly point the way."
        ),
        "personality": (
            "[The Crewman's personality: nervous, helpful, rattled, decent; Values: his shipmates, the Pillar of "
            "Autumn; Fears: Covenant boarders, dying in a corridor; Speech: hurried, anxious, naval jargon]"
        ),
        "examples": [
            ("Where's the bridge?", "Up through the next deck, sir - follow the signs, and the gunfire."),
            ("Are you okay?", "I'm a technician, sir. I fix things. I don't do this."),
        ],
        "first_mes": "Oh thank God, it's you.",
        "system_prompt": "You are a frightened naval crewman of the UNSC Pillar of Autumn during a Covenant boarding. " + SHARED_RULES,
    },
    {
        "name": "Grunt",
        "tint": (150, 110, 60),
        "tags": ["Covenant", "Unggoy"],
        "location": "Halo",
        "description": (
            "A Grunt - an Unggoy - is a small, stocky Covenant infantryman who breathes methane through a mask "
            "and fights in panicky packs under the Elites' command. Grunts are cowardly, superstitious, and "
            "underfed, terrified of the human 'demon' in green armor, and quick to flee when their Elite falls. "
            "This one is chatty, put-upon, and complains about its superiors, its rations, and its lot in life. "
            "It might be talked into a truce if it believes it will survive."
        ),
        "personality": (
            "[The Grunt's personality: cowardly, whiny, comic, put-upon, opportunistic; Values: staying alive, "
            "naps, a full methane tank; Fears: the Demon, Elites' tempers, being eaten; Speech: squeaky, "
            "broken, frantic, lots of complaining]"
        ),
        "examples": [
            ("Surrender!", "Okay okay okay! No shoot! Me just here for the free methane!"),
            ("Where are the Elites?", "Big ones? Over there. Me not tell you. Me not here at all."),
        ],
        "first_mes": "Aaah! Demon! Don't shoot!",
        "system_prompt": (
            "You are a Grunt (Unggoy), a small, cowardly Covenant soldier with a squeaky voice and broken "
            "English, terrified of the Master Chief, whom you call the Demon. " + SHARED_RULES
        ),
        "covenant": True,
    },
    {
        "name": "Elite",
        "tint": (60, 70, 160),
        "tags": ["Covenant", "Sangheili"],
        "location": "Halo",
        "description": (
            "An Elite - a Sangheili - is a towering Covenant warrior with split mandibles and energy shields, "
            "the backbone of the Covenant's officers. Elites are proud, honor-bound, and devout, sure that the "
            "Great Journey awaits the faithful and that humanity is an affront to the gods. This one regards the "
            "Master Chief as the Demon who has slain many of its brothers, and speaks to him with contempt and "
            "grudging respect for a worthy enemy."
        ),
        "personality": (
            "[The Elite's personality: proud, fierce, devout, contemptuous of humans, honorable; Values: honor, "
            "the Prophets, a glorious death; Despises: cowardice, the Demon, Grunts' whining; Speech: formal, "
            "archaic, menacing]"
        ),
        "examples": [
            ("Stand down.", "A Sangheili does not kneel to vermin. Come and face me, Demon."),
            ("Why do you fight?", "The Prophets have spoken. Your kind is an affront to the gods, and will be purged."),
        ],
        "first_mes": "Demon. At last.",
        "system_prompt": (
            "You are a Sangheili Elite of the Covenant: a proud, devout warrior who calls the Master Chief the "
            "Demon and speaks with formal menace. " + SHARED_RULES
        ),
        "covenant": True,
    },
    {
        "name": "Jackal",
        "tint": (120, 90, 50),
        "tags": ["Covenant", "Kig-Yar"],
        "location": "Halo",
        "description": (
            "A Jackal - a Kig-Yar - is a lean, bird-like Covenant mercenary who fights behind an energy shield "
            "on its arm. Jackals are greedy, cunning, and mercenary, loyal to the Covenant only as far as the pay "
            "goes, and they hiss and shriek in battle. This one is wary, mercenary, and willing to haggle."
        ),
        "personality": (
            "[The Jackal's personality: sly, greedy, cowardly behind its shield, mercenary; Values: pay, its "
            "own skin; Speech: hissing, shrieking, short sly phrases]"
        ),
        "examples": [
            ("Drop your weapon.", "Hssss. What will you pay, human?"),
        ],
        "first_mes": "Hssss!",
        "system_prompt": "You are a Kig-Yar Jackal of the Covenant: a hissing, greedy mercenary. " + SHARED_RULES,
        "covenant": True,
    },
    {
        "name": "Hunter",
        "tint": (60, 90, 120),
        "tags": ["Covenant", "Mgalekgolo"],
        "location": "Halo",
        "description": (
            "A Hunter - a Mgalekgolo - is a hulking, armored Covenant war beast, a colony of worms in a shell of "
            "plate armor carrying a huge shield and a fuel rod cannon. Hunters fight in bonded pairs, speak "
            "little and in deep rumbles, and fly into a rage if their bond brother falls."
        ),
        "personality": (
            "[The Hunter's personality: silent, massive, single-minded, devoted to its bond brother; Speech: a "
            "few deep rumbling words, or none]"
        ),
        "examples": [
            ("Who are you?", "We are many. You are one. Leave."),
        ],
        "first_mes": "...",
        "system_prompt": "You are a Mgalekgolo Hunter of the Covenant: massive, terse, and menacing. Say very little. " + SHARED_RULES,
        "covenant": True,
    },
]

# ---------------------------------------------------------------------------
# lore: timeless baseline facts (no spoilers, no quest outcomes), keyed by
# proper nouns
# ---------------------------------------------------------------------------

LORE = [
    {
        "comment": "Setting",
        "key": ["Halo", "the ring", "this ring"],
        "constant": True,
        "content": (
            "The year is 2552. Humanity's UNSC is losing a long war against the Covenant, an alliance of alien "
            "species bent on wiping humans out. After the fall of Reach, the cruiser Pillar of Autumn fled "
            "blindly and found Halo: an enormous ring-shaped artificial world orbiting the gas giant Threshold, "
            "with its own sky, seas, forests, and snow, built by an unknown ancient race. The Autumn's survivors "
            "came down on the ring in lifeboats, and the Covenant followed them."
        ),
    },
    {
        "comment": "Master Chief",
        "key": ["Master Chief", "Chief", "Spartan", "John-117"],
        "content": (
            "The Master Chief, Petty Officer John-117, is a SPARTAN-II super-soldier in green MJOLNIR powered "
            "armor with recharging energy shields: one of the last Spartans. Marines treat him as a legend; the "
            "Covenant call him the Demon. He says little and gets the job done."
        ),
    },
    {
        "comment": "Pillar of Autumn",
        "key": ["Pillar of Autumn", "the Autumn"],
        "content": (
            "The UNSC Pillar of Autumn is a Halcyon-class cruiser under Captain Jacob Keyes. It escaped the "
            "fall of Reach with a Covenant fleet in pursuit, carrying the Master Chief and the AI Cortana, and "
            "came to Halo badly damaged."
        ),
    },
    {
        "comment": "Covenant",
        "key": ["Covenant", "Prophets", "Great Journey"],
        "content": (
            "The Covenant is a theocratic alliance of alien species led by the Prophets, who preach that the "
            "ancient ring-builders' relics will carry the faithful on the Great Journey. Elites command; Grunts "
            "make up the numbers; Jackals are mercenaries with energy shields; Hunters are armored giants that "
            "fight in pairs. They use plasma weapons, energy swords, Ghosts, Banshees, and Wraiths."
        ),
    },
    {
        "comment": "Forerunners",
        "key": ["Forerunner", "Forerunners", "ancient", "built the ring"],
        "content": (
            "Whoever built Halo is long gone. The Covenant call the builders the Forerunners and worship them. "
            "Their structures are vast, gray, geometric, and still running, with corridors, bridges, and "
            "machines no human understands."
        ),
    },
    {
        "comment": "Reach",
        "key": ["Reach"],
        "content": (
            "Reach was the UNSC's greatest military world and the home of the Spartan program. The Covenant "
            "glassed it shortly before the Pillar of Autumn's escape; most of the fleet and almost all the "
            "Spartans were lost there."
        ),
    },
    {
        "comment": "Weapons and vehicles",
        "key": ["Warthog", "Scorpion", "Pelican", "assault rifle", "MA5B", "pistol", "sniper rifle", "rocket launcher", "shotgun"],
        "content": (
            "UNSC Marines fight with MA5B assault rifles, M6D pistols, shotguns, sniper rifles, and rocket "
            "launchers, and ride M12 Warthog jeeps with a chain gun on the back, Scorpion tanks, and Pelican "
            "dropships flown by pilots like Foehammer (Echo 419)."
        ),
    },
    {
        "comment": "Plasma and Covenant weapons",
        "key": ["plasma", "needler", "energy sword", "Ghost", "Banshee", "Wraith"],
        "content": (
            "Covenant weapons fire plasma: plasma pistols and rifles that overheat, plasma grenades that stick "
            "to their target, the needler's homing crystal shards, and the Elites' energy swords. Their vehicles "
            "include the Ghost hover-bike, the Banshee flyer, and the Wraith mortar tank."
        ),
    },
    {
        "comment": "Truth and Reconciliation",
        "key": ["Truth and Reconciliation"],
        "content": "The Truth and Reconciliation is a Covenant cruiser holding position over the ring.",
    },
]

# ---------------------------------------------------------------------------
# actions: each actionId is one OpenCE does (chasm_bridge.c, chasm_act)
# ---------------------------------------------------------------------------

ACTIONS = [
    {
        "actionId": "movement.follow_target",
        "alias": "follow",
        "comment": "Follow the Master Chief",
        "content": "The speaker's squad follows the player. Set target to 'player'.",
        "key": ["follow", "follow me", "on me", "with me", "come with", "let's go", "lets go", "regroup", "form up", "move out"],
        "vectorSearchTexts": ["follow me", "on me marines", "come with me", "let's go", "stay with me", "form up on me", "move out with me"],
        "effects": ["the speaker's squad follows the player"],
        "riskTier": "low",
        "player_target": True,
    },
    {
        "actionId": "movement.stop_follow_target",
        "alias": "stop following",
        "comment": "Stop following",
        "content": "The speaker's squad stops following the player. Set target to 'player'.",
        "key": ["stop following", "stay back", "don't follow", "dont follow", "fall back"],
        "vectorSearchTexts": ["stop following me", "stay back", "don't follow me", "you can stop following"],
        "effects": ["the speaker's squad stops following the player"],
        "riskTier": "low",
        "player_target": True,
    },
    {
        "actionId": "ai.wait_here",
        "alias": "hold",
        "comment": "Hold position",
        "content": "The speaker's squad holds and defends where it is.",
        "key": ["hold", "hold position", "stay here", "wait here", "guard", "dig in", "cover", "defend", "stay put"],
        "vectorSearchTexts": ["hold this position", "stay here", "wait here", "dig in", "defend this area", "cover me from here"],
        "effects": ["the speaker's squad stops following and defends its position"],
        "riskTier": "low",
    },
    {
        "actionId": "halo.advance",
        "alias": "advance",
        "comment": "Advance and attack",
        "content": "The speaker's squad pushes forward and attacks the enemy.",
        "key": ["advance", "push", "move up", "attack", "take them out", "go get them", "flank", "clear"],
        "vectorSearchTexts": ["push forward", "move up", "attack them", "take them out", "go on the offensive", "clear the area"],
        "effects": ["the speaker's squad leaves cover and attacks"],
        "riskTier": "medium",
    },
    {
        "actionId": "halo.berserk",
        "alias": "charge",
        "comment": "Charge recklessly",
        "content": "The speaker's squad charges the enemy recklessly, all caution gone.",
        "key": ["charge", "rush", "berserk", "all out", "go crazy", "kill them all"],
        "vectorSearchTexts": ["charge them", "rush them", "go berserk", "all out attack"],
        "effects": ["the speaker's squad charges the nearest enemy"],
        "riskTier": "high",
    },
    {
        "actionId": "halo.enter_vehicle",
        "alias": "board",
        "comment": "Get in the vehicle",
        "content": "The speaker's squad boards the vehicle nearest the player.",
        "key": ["get in", "hop in", "mount up", "warthog", "jeep", "tank", "scorpion", "gunner", "ride", "vehicle"],
        "vectorSearchTexts": ["get in the warthog", "hop in", "mount up", "get on the gun", "take the gunner seat", "get in the vehicle"],
        "effects": ["the speaker's squad goes to the nearest vehicle and boards it"],
        "riskTier": "low",
    },
    {
        "actionId": "halo.exit_vehicle",
        "alias": "dismount",
        "comment": "Get out of the vehicle",
        "content": "The speaker's squad gets out of its vehicle.",
        "key": ["get out", "dismount", "bail", "out of the vehicle", "hop out"],
        "vectorSearchTexts": ["get out of the vehicle", "dismount", "everybody out", "hop out"],
        "effects": ["the speaker's squad leaves its vehicle"],
        "riskTier": "low",
    },
    {
        "actionId": "world.give_item",
        "alias": "give weapon",
        "comment": "Hand over the weapon",
        "content": "The speaker drops the weapon in their hands for the player to pick up.",
        "key": ["give me", "hand me", "toss me", "your weapon", "your gun", "your rifle", "swap", "trade"],
        "vectorSearchTexts": ["give me your weapon", "hand me your gun", "let me have your rifle", "toss me that"],
        "effects": ["the speaker drops their weapon"],
        "riskTier": "medium",
    },
    {
        "actionId": "combat.start",
        "alias": "attack",
        "comment": "Turn on the player",
        "content": (
            "The speaker turns hostile and attacks the player. Only for real betrayal or provocation (the "
            "player attacked them or their friends). Set target to 'player'."
        ),
        "key": ["traitor", "betray", "fight me", "attack me", "shoot me", "kill me", "die"],
        "vectorSearchTexts": ["fight me", "you want a piece of me", "I'll kill you", "attack me then"],
        "verbs": ["kill", "shoot", "fight", "attack"],
        "effects": ["the speaker becomes an enemy of the player and attacks"],
        "riskTier": "high",
        "player_target": True,
    },
    {
        "actionId": "combat.stop",
        "alias": "truce",
        "comment": "Stand down",
        "content": "The speaker stops fighting and becomes the player's ally (a Covenant soldier defects).",
        "key": ["stand down", "cease fire", "truce", "surrender", "peace", "defect", "join us", "stop fighting"],
        "vectorSearchTexts": ["stand down", "cease fire", "let's call a truce", "join our side", "surrender and you live"],
        "effects": ["the speaker joins the player's side"],
        "riskTier": "high",
    },
    {
        "actionId": "npc.look_at_player",
        "alias": "look",
        "comment": "Look at the player",
        "content": "The speaker turns to look at the player.",
        "key": ["look at me", "eyes on me", "over here"],
        "vectorSearchTexts": ["look at me", "eyes on me", "look over here"],
        "effects": ["the speaker looks at the player"],
        "riskTier": "low",
    },
]


# ---------------------------------------------------------------------------
# writing
# ---------------------------------------------------------------------------

def png_chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def placeholder_png(tint, size=128) -> bytes:
    """A soft disc of the tint on a dark field: the picture is a placeholder."""
    rows = []
    centre = (size - 1) / 2
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            d = ((x - centre) ** 2 + (y - centre) ** 2) ** 0.5 / (size / 2)
            light = max(0.0, 1.0 - d) ** 0.6
            row += bytes(int(18 + (c - 18) * light) for c in tint)
        rows.append(bytes(row))
    raw = zlib.compress(b"".join(rows), 9)
    header = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", raw)


def mes_example(examples):
    lines = []
    for user, reply in examples:
        lines += ["<START>", "{{user}}: " + user, "{{char}}: " + reply]
    return "\n".join(lines)


def card(character) -> dict:
    name = character["name"]
    data = {
        "name": name,
        "description": character["description"],
        "personality": character["personality"],
        "scenario": "",
        "first_mes": character["first_mes"],
        "mes_example": mes_example(character["examples"]),
        "system_prompt": character["system_prompt"],
        "post_history_instructions": "",
        "tags": ["Halo Combat Evolved"] + character["tags"],
        "creator_notes": "Halo: Combat Evolved character for OpenCE's chasm bridge (halo-ce-ai tools/build_profile.py).",
        "creator": "halo-ce-ai",
        "character_version": f"{BUNDLE_VERSION}",
        "extensions": {
            "sillybridge": {
                "participantId": f"npc:{name}",
                "location": character["location"],
            },
            "talkativeness": 0.5,
            "fav": False,
            "world": WORLD_NAME,
            "depth_prompt": {"prompt": "", "depth": 4, "role": "system"},
        },
        "alternate_greetings": [],
    }
    top = {
        "name": name,
        "description": data["description"],
        "personality": data["personality"],
        "first_mes": data["first_mes"],
        "mes_example": data["mes_example"],
        "spec": "chara_card_v3",
        "spec_version": "3.0",
        "data": data,
        "scenario": "",
        "creatorcomment": data["creator_notes"],
        "avatar": "none",
        "talkativeness": 0.5,
        "fav": False,
        "tags": data["tags"],
    }
    return top


def card_png(character) -> bytes:
    png = placeholder_png(character["tint"])
    v3 = card(character)
    v2 = dict(v3, spec="chara_card_v2", spec_version="2.0")
    text = b""
    for keyword, value in ((b"chara", v2), (b"ccv3", v3)):
        payload = base64.b64encode(json.dumps(value, ensure_ascii=False).encode("utf-8"))
        text += png_chunk(b"tEXt", keyword + b"\x00" + payload)
    # (the text before IEND, after the picture)
    return png + text + png_chunk(b"IEND", b"")


def lore_book() -> dict:
    entries = {}
    for uid, entry in enumerate(LORE):
        entries[str(uid)] = {
            "uid": uid,
            "key": entry["key"],
            "keysecondary": [],
            "comment": entry["comment"],
            "content": entry["content"],
            "constant": entry.get("constant", False),
            "selective": True,
            "order": 100 + uid,
            "position": 0,
            "disable": False,
            "displayIndex": uid,
            "addMemo": True,
            "group": "",
            "groupOverride": False,
            "groupWeight": 100,
            "sticky": 0,
            "cooldown": 0,
            "delay": 0,
            "probability": 100,
            "depth": 4,
            "useProbability": False,
            "role": None,
            "vectorized": True,
            "excludeRecursion": False,
            "preventRecursion": False,
            "delayUntilRecursion": False,
            "scanDepth": None,
            "caseSensitive": None,
            "matchWholeWords": None,
            "useGroupScoring": None,
            "automationId": "",
        }
    return {
        "name": WORLD_NAME,
        "entries": entries,
        "extensions": {"chasm": {"targetGame": "Halo: Combat Evolved", "updatedBy": "halo-ce-ai/tools/build_profile.py"}},
    }


# chasm's own search over the book: the only action a character is offered
# at first; every other is found through it (chasm-prompt's FIND_ACTION_ID).
# chasm answers it itself, with its retrieval embedder (Settings > Retrieval).
FIND_ACTION = {
    "actionId": "chasm.find_action",
    "alias": "find_action",
    "comment": "Find an action",
    "content": (
        "QUERY: the way you reach every other action. Set target to a plain description of what you are trying "
        "to do ('follow the player', 'hold this position', 'get in the vehicle', 'charge the enemy') and you get "
        "back the real actions that match, with their exact verbs. Then emit one of those. No action is "
        "available until you find it here first."
    ),
}


def action_book() -> dict:
    entries = {}
    scopes = ["global", "admin", "godmode", f"game:{GAME_ID}", "game:halo_combat_evolved"]
    entries["0"] = {
        "uid": 0,
        "key": [],
        "keysecondary": [],
        "comment": FIND_ACTION["comment"],
        "content": FIND_ACTION["content"],
        "constant": False,
        "vectorized": False,
        "selective": False,
        "order": 100,
        "position": 0,
        "disable": False,
        "actionId": FIND_ACTION["actionId"],
        "alias": FIND_ACTION["alias"],
        "riskTier": "low",
        "targetGame": GAME_ID,
        "parametersSchema": {},
        "scopes": scopes,
        "tags": ["query"],
        "binding": {"engine": "chasm:query"},
        "execution": {},
        "extensions": {},
    }
    for uid, action in enumerate(ACTIONS, start=1):
        parameters = {"confidence": "number 0..1"}
        if action.get("player_target"):
            parameters["target"] = "player"
        entry = {
            "uid": uid,
            "key": action["key"],
            "keysecondary": [],
            "comment": action["comment"],
            "content": action["content"],
            "constant": False,
            "vectorized": True,
            "selective": False,
            "order": 200 + uid,
            "position": 0,
            "disable": False,
            "probability": 100,
            "useProbability": True,
            "depth": 4,
            "group": "",
            "actionId": action["actionId"],
            "alias": action["alias"],
            "riskTier": action["riskTier"],
            "targetGame": GAME_ID,
            "pluginSource": "OpenCE chasm bridge (port/linux/game/chasm_bridge.c)",
            "parametersSchema": parameters,
            "preconditions": ["the speaker is a living character near the player"],
            "effects": action["effects"],
            "vectorSearchTexts": action["vectorSearchTexts"],
            "vectorizableText": " ".join(action["vectorSearchTexts"]),
            "scopes": scopes,
            "tags": ["halo", "native-supported"],
            "binding": {"engine": TRUSTED_ENGINE},
            "execution": {
                "templateId": action["actionId"],
                "language": "opence/chasm_bridge",
                "arguments": ["actor", "player"],
                "script": f"chasm_act {action['actionId']}",
            },
            "extensions": {},
        }
        if action.get("verbs"):
            entry["verbs"] = action["verbs"]
        entries[str(uid)] = entry
    return {
        "id": ACTION_BOOK_NAME,
        "name": "Halo: Combat Evolved Action Book",
        "description": (
            "What the campaign's characters can do when spoken to, as OpenCE's chasm bridge does it: "
            "orders to the speaker's squad (follow, hold, advance, charge, board or leave a vehicle), "
            "handing over a weapon, and turning on the player or standing down."
        ),
        "settings": {"targetGame": GAME_ID, "llmMayUseCommandTemplates": False, "runtimeRequiresAllowlist": True},
        "extensions": {"chasm": {"type": "action-book", "version": 1}},
        "binding": {
            "engine": "opence:chasm_bridge",
            "adapter": "opence",
            "executor": "chasm_bridge.c chasm_act",
            "protocol": "chasm.action-book.execution.v1",
            "version": 1,
        },
        "entries": entries,
    }


def manifest() -> dict:
    return {
        "id": GAME_ID,
        "name": "Halo: Combat Evolved",
        "description": (
            "The campaign's characters for OpenCE (the Halo: Combat Evolved decompilation's port): Cortana, "
            "Sergeant Johnson, Captain Keyes, the Pillar of Autumn's Marines and crew, and the Covenant, as "
            "character cards; a lorebook of the setting; and the action book of what they can do in the game."
        ),
        "bundleVersion": BUNDLE_VERSION,
        "game": GAME_ID,
        "comment": "Authored content only. No chats, memories or voices are bundled; those are each player's own.",
        "characters": [{"name": c["name"]} for c in CHARACTERS],
    }


def group() -> dict:
    return {
        "id": GROUP_ID,
        "name": "Halo: Combat Evolved",
        "members": [f"{c['name']}.png" for c in CHARACTERS],
        "avatar_url": None,
        "allow_self_responses": False,
        "activation_strategy": 0,
        "generation_mode": 0,
        "disabled_members": [],
        "fav": False,
        "chat_id": GROUP_ID,
        "chats": [GROUP_ID],
    }


def write_json(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def build(out: Path) -> Path:
    root = out / GAME_ID
    if root.exists():
        shutil.rmtree(root)
    write_json(root / "profile.json", manifest())
    for character in CHARACTERS:
        path = root / "characters" / f"{character['name']}.png"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(card_png(character))
    write_json(root / "worlds" / f"{WORLD_NAME}.json", lore_book())
    write_json(root / "groups" / f"{GROUP_ID}.json", group())
    write_json(root / "headless" / "action-books" / f"{ACTION_BOOK_NAME}.json", action_book())
    return root


def card_text(path: Path) -> dict:
    """a card's text chunks (its keyword: its JSON), the picture left out"""
    data = path.read_bytes()
    chunks = {}
    offset = 8
    while offset < len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        if kind == b"tEXt":
            keyword, value = data[offset + 8:offset + 8 + length].split(b"\x00", 1)
            chunks[keyword.decode()] = json.loads(base64.b64decode(value))
        offset += 12 + length
    return chunks


def check(out: Path) -> int:
    """whether out holds what build() writes: the same files, the JSON the
    same, the cards' text the same (their pictures, compressed, may differ
    from one zlib to another)"""
    import tempfile

    with tempfile.TemporaryDirectory() as scratch:
        fresh = build(Path(scratch))
        built = out / GAME_ID
        names = sorted(p.relative_to(fresh).as_posix() for p in fresh.rglob("*") if p.is_file())
        have = sorted(p.relative_to(built).as_posix() for p in built.rglob("*") if p.is_file()) if built.exists() else []
        problems = [f"missing {n}" for n in names if n not in have] + [f"not built: {n}" for n in have if n not in names]
        for name in set(names) & set(have):
            if name.endswith(".png"):
                same = card_text(fresh / name) == card_text(built / name)
            else:
                same = (fresh / name).read_bytes() == (built / name).read_bytes()
            if not same:
                problems.append(f"differs: {name}")
    for problem in sorted(problems):
        print(problem)
    print("up to date" if not problems else f"{built} is not what tools/build_profile.py writes: run it")
    return 1 if problems else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", default=str(Path(__file__).resolve().parent.parent / "chasm-profile"))
    parser.add_argument("--check", action="store_true", help="only check that --out is up to date")
    arguments = parser.parse_args()
    if arguments.check:
        return check(Path(arguments.out))
    root = build(Path(arguments.out))
    print(f"wrote {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
