#!/usr/bin/env python3
"""Reject a verifier that rebinds the repository path inside the extract directory."""
import ast
from pathlib import Path

source = Path(__file__).resolve().parent.parent / "scripts/verify-package.py"
tree = ast.parse(source.read_text())
assignments = []


class Visitor(ast.NodeVisitor):
    def visit_Assign(self, node):
        for target in node.targets:
            if isinstance(target, ast.Name) and target.id == "root":
                assignments.append(node.lineno)
        self.generic_visit(node)

    def visit_AnnAssign(self, node):
        target = node.target
        if isinstance(target, ast.Name) and target.id == "root":
            assignments.append(node.lineno)
        self.generic_visit(node)

    def visit_AugAssign(self, node):
        target = node.target
        if isinstance(target, ast.Name) and target.id == "root":
            assignments.append(node.lineno)
        self.generic_visit(node)


Visitor().visit(tree)
assert assignments, "repository path root is not assigned"
assert len(assignments) == 1, "root is assigned more than once at lines " + str(assignments)
assigned = None
for node in tree.body:
    if isinstance(node, ast.Assign):
        for target in node.targets:
            if isinstance(target, ast.Name) and target.id == "root":
                assigned = node
assert assigned is not None and assigned.lineno == assignments[0]
lines = [line.strip() for line in source.read_text().splitlines()]
assert "extract_root = Path(temporary)" in lines
assert "root = Path(temporary)" not in lines
print("ok 1 - verify-package.py assigns repository root once and keeps the extract directory separate")
