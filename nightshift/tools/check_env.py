#!/usr/bin/env python3
"""Verify the deployed parser can actually parse supported source code."""
import sys


def main():
    try:
        from tree_sitter_language_pack import get_parser
        for language, source in (
            ('c', b'int main(void) { return 0; }'),
            ('cpp', b'class Example { public: int value() { return 1; } };'),
            ('python', b'def example():\n    return 1\n'),
        ):
            tree = get_parser(language).parse(source)
            if tree.root_node.has_error:
                raise RuntimeError(f'{language} parser smoke check failed')
    except Exception as error:
        print(f'code-index parser environment is unusable: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
