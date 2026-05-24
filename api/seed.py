import os
import struct
import sqlite3
from database import DB_PATH, init_db
from auth import hash_password
from datetime import datetime, timezone

MDB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "mydb")
MDB_FORMAT = "16s24s"  # matches struct MdbRec { char name[16]; char msg[24]; }

USERS = [
    ("hgranger",    "crookshanks"),
    ("rweasley",    "chessmaster"),
    ("hansolo",     "chewie"),
    ("leiaorg",     "alderaan"),
    ("luna_l",      "nargles"),
    ("aryastark",   "needle"),
    ("samgamgee",   "potatoes"),
    ("c3po",        "r2d2"),
    ("tonyStark",   "jarvis"),
    ("natashaR",    "redroom"),
    ("brucewayne",  "alfred"),
    ("dianaprince", "themyscira"),
    ("spock_v",     "vulcan"),
    ("walterw",     "heisenberg"),
    ("leslie_k",    "waffles"),
    ("hannibal_l",  "chianti"),
]

ENTRIES = [
    ("hgranger",    "Hermione Granger",   "it's LeviOsa, not LevioSA"),
    ("rweasley",    "Ron Weasley",        "why spiders, why couldn't it be follow the butterflies"),
    ("hansolo",     "Han Solo",           "never tell me the odds"),
    ("leiaorg",     "Leia Organa",        "help me Obi-Wan Kenobi, you're my only hope"),
    ("luna_l",      "Luna Lovegood",      "things we lose have a way of coming back to us in the end"),
    ("aryastark",   "Arya Stark",         "not today"),
    ("samgamgee",   "Samwise Gamgee",     "there's some good in this world, and it's worth fighting for"),
    ("c3po",        "C-3PO",              "we're doomed"),
    ("tonyStark",   "Tony Stark",         "I am Iron Man"),
    ("natashaR",    "Natasha Romanoff",   "I've got red in my ledger, I'd like to wipe it out"),
    ("brucewayne",  "Bruce Wayne",        "I'm Batman"),
    ("dianaprince", "Diana Prince",       "I am the man who can"),
    ("spock_v",     "Spock",              "live long and prosper"),
    ("walterw",     "Walter White",       "I am the danger"),
    ("leslie_k",    "Leslie Knope",       "I have a lot of ideas and I have a lot of energy and I care a lot"),
    ("hannibal_l",  "Hannibal Lecter",    "I'm having an old friend for dinner"),
]

# name[16] and msg[24]; struct.pack silently truncates values that are too long
MDB_ENTRIES = [
    ("Han Solo",       "never tell me the odds"),
    ("Arya Stark",     "not today"),
    ("Bruce Wayne",    "I'm Batman"),
    ("Tony Stark",     "I am Iron Man"),
    ("C-3PO",          "we're doomed"),
    ("Spock",          "live long and prosper"),
    ("Walter White",   "I am the danger"),
    ("Diana Prince",   "I am the man who can"),
    ("Ron Weasley",    "follow the spiders"),
    ("Luna Lovegood",  "keep an open mind"),
    ("Leia Organa",    "help me Obi-Wan"),
    ("Samwise Gamgee", "I can carry you"),
    ("Leslie Knope",   "waffles are life"),
    ("Hannibal Lecter","come for dinner"),
]


def seed_sqlite():
    init_db()
    conn = sqlite3.connect(DB_PATH)
    conn.execute("PRAGMA foreign_keys = ON")
    conn.execute("DELETE FROM entries")
    conn.execute("DELETE FROM users")
    conn.execute("DELETE FROM sqlite_sequence WHERE name IN ('users', 'entries')")

    now = datetime.now(timezone.utc).isoformat()
    user_ids = {}
    for username, password in USERS:
        cursor = conn.execute(
            "INSERT INTO users (username, password_hash, created_at) VALUES (?, ?, ?)",
            (username, hash_password(password), now),
        )
        user_ids[username] = cursor.lastrowid

    for username, name, message in ENTRIES:
        conn.execute(
            "INSERT INTO entries (user_id, name, message, created_at, updated_at) VALUES (?, ?, ?, ?, ?)",
            (user_ids[username], name, message, now, now),
        )

    conn.commit()
    conn.close()
    print(f"Seeded {len(USERS)} users and {len(ENTRIES)} entries into directory.db.")


def seed_mdb():
    with open(MDB_PATH, "wb") as fp:
        for name, msg in MDB_ENTRIES:
            record = struct.pack(MDB_FORMAT,
                name.encode()[:15].ljust(16, b"\x00"),
                msg.encode()[:23].ljust(24, b"\x00"),
            )
            fp.write(record)
    print(f"Seeded {len(MDB_ENTRIES)} records into data/mydb.")


if __name__ == "__main__":
    seed_sqlite()
    seed_mdb()
