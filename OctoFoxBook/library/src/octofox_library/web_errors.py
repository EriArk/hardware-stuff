"""Shared HTTP errors, with identical identity under both imports and python -m."""


class WebError(Exception):
    def __init__(self, status: int, message: str):
        self.status, self.message = status, message
