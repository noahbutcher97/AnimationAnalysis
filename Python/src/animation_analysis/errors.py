"""Recoverable invalid or incomplete observation evidence."""


class EvidenceError(ValueError):
    """Evidence cannot support the requested measurement or comparison."""
