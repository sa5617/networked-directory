from typing import Optional

from pydantic import BaseModel


class UserCreate(BaseModel):
    username: str
    password: str


class UserResponse(BaseModel):
    id: int
    username: str
    created_at: str


class LoginRequest(BaseModel):
    username: str
    password: str


class Token(BaseModel):
    access_token: str
    token_type: str


class EntryCreate(BaseModel):
    name: str
    message: str


class EntryUpdate(BaseModel):
    name: Optional[str] = None
    message: Optional[str] = None


class EntryResponse(BaseModel):
    id: int
    user_id: int
    username: str
    name: str
    message: str
    created_at: str
    updated_at: str
