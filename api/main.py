from contextlib import asynccontextmanager
from datetime import datetime, timezone

from fastapi import Depends, FastAPI, HTTPException, Query, Response
from fastapi.middleware.cors import CORSMiddleware

from auth import create_token, get_current_user, hash_password, verify_password
from database import get_db, init_db
from models import (
    EntryCreate,
    EntryResponse,
    EntryUpdate,
    LoginRequest,
    Token,
    UserCreate,
    UserResponse,
)


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    yield


app = FastAPI(lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

ENTRY_SELECT = """
    SELECT entries.id, entries.user_id, users.username,
           entries.name, entries.message,
           entries.created_at, entries.updated_at
    FROM entries
    JOIN users ON entries.user_id = users.id
"""


def get_owned_entry(db, entry_id: int, user_id: int):
    row = db.execute(
        "SELECT user_id FROM entries WHERE id = ?", (entry_id,)
    ).fetchone()
    if row is None:
        raise HTTPException(status_code=404, detail="entry not found")
    if row["user_id"] != user_id:
        raise HTTPException(status_code=403, detail="not your entry")
    return row


@app.post("/api/register", status_code=201, response_model=UserResponse)
async def register(body: UserCreate, db=Depends(get_db)):
    existing = db.execute(
        "SELECT id FROM users WHERE username = ?", (body.username,)
    ).fetchone()
    if existing:
        raise HTTPException(status_code=409, detail="username taken")
    db.execute(
        "INSERT INTO users (username, password_hash) VALUES (?, ?)",
        (body.username, hash_password(body.password)),
    )
    db.commit()
    row = db.execute(
        "SELECT id, username, created_at FROM users WHERE username = ?",
        (body.username,),
    ).fetchone()
    return UserResponse(**dict(row))


@app.post("/api/login", response_model=Token)
async def login(body: LoginRequest, db=Depends(get_db)):
    row = db.execute(
        "SELECT id, username, password_hash FROM users WHERE username = ?",
        (body.username,),
    ).fetchone()
    if not row or not verify_password(body.password, row["password_hash"]):
        raise HTTPException(status_code=401, detail="invalid credentials")
    token = create_token(row["id"], row["username"])
    return Token(access_token=token, token_type="bearer")


@app.get("/api/entries", response_model=list[EntryResponse])
async def list_entries(search: str = Query(None), db=Depends(get_db)):
    if search:
        rows = db.execute(
            ENTRY_SELECT + " WHERE entries.name LIKE ? OR entries.message LIKE ?"
            " ORDER BY entries.created_at DESC",
            (f"%{search}%", f"%{search}%"),
        ).fetchall()
    else:
        rows = db.execute(
            ENTRY_SELECT + " ORDER BY entries.created_at DESC"
        ).fetchall()
    return [EntryResponse(**dict(row)) for row in rows]


@app.post("/api/entries", status_code=201, response_model=EntryResponse)
async def create_entry(
    body: EntryCreate,
    db=Depends(get_db),
    user=Depends(get_current_user),
):
    now = datetime.now(timezone.utc).isoformat()
    cursor = db.execute(
        "INSERT INTO entries (user_id, name, message, created_at, updated_at)"
        " VALUES (?, ?, ?, ?, ?)",
        (user["user_id"], body.name, body.message, now, now),
    )
    db.commit()
    row = db.execute(
        ENTRY_SELECT + " WHERE entries.id = ?", (cursor.lastrowid,)
    ).fetchone()
    return EntryResponse(**dict(row))


@app.put("/api/entries/{entry_id}", response_model=EntryResponse)
async def update_entry(
    entry_id: int,
    body: EntryUpdate,
    db=Depends(get_db),
    user=Depends(get_current_user),
):
    get_owned_entry(db, entry_id, user["user_id"])

    updates = {k: v for k, v in {"name": body.name, "message": body.message}.items()
               if v is not None}
    if updates:
        now = datetime.now(timezone.utc).isoformat()
        set_clause = ", ".join(f"{k} = ?" for k in updates)
        db.execute(
            f"UPDATE entries SET {set_clause}, updated_at = ? WHERE id = ?",
            (*updates.values(), now, entry_id),
        )
        db.commit()

    updated = db.execute(
        ENTRY_SELECT + " WHERE entries.id = ?", (entry_id,)
    ).fetchone()
    return EntryResponse(**dict(updated))


@app.delete("/api/entries/{entry_id}", status_code=204)
async def delete_entry(
    entry_id: int,
    db=Depends(get_db),
    user=Depends(get_current_user),
):
    get_owned_entry(db, entry_id, user["user_id"])
    db.execute("DELETE FROM entries WHERE id = ?", (entry_id,))
    db.commit()
    return Response(status_code=204)
