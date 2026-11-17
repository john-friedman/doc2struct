"""Generic HTML table rendering for ParseBench, including opt-in cell styles.

Keeps doc2struct's plain matrix strings and repeated spanning cells intact.
No benchmark annotations or expected answers are used during conversion.
"""
from html import escape


def _paragraph(value):
    return "<p>" + escape(value).replace("\n", "<br>\n") + "</p>" if value else ""


def _cell(value, runs):
    if not runs:
        return escape(value or "")
    tags = ((1, "strong"), (2, "em"), (4, "u"), (128, "s"))
    parts = []
    for run in runs:
        flags = run.get("flags", 0)
        opened = [tag for bit, tag in tags if flags & bit]
        parts.append("".join(f"<{tag}>" for tag in opened)
                     + escape(run.get("text", ""))
                     + "".join(f"</{tag}>" for tag in reversed(opened)))
    return "".join(parts)


def table_html(row, current_title):
    blocks = []
    if row.get("text") and row["text"] != current_title:
        blocks.append(_paragraph(row["text"]))
    blocks.append(_paragraph(row.get("preamble")))
    styles = {(cell["row"], cell["col"]): cell["runs"]
              for cell in row.get("cell_runs", [])}
    table = ["<table>", "<tbody>"]
    for r, cells in enumerate(row.get("matrix") or []):
        table.append("<tr>" + "".join(
            "<td>" + _cell(value, styles.get((r, c))) + "</td>"
            for c, value in enumerate(cells)) + "</tr>")
    table.extend(["</tbody>", "</table>"])
    blocks.append("\n".join(table))
    for marker, text in row.get("footnotes") or []:
        blocks.append(_paragraph(" ".join(part for part in (marker, text) if part)))
    blocks.append(_paragraph(row.get("postamble")))
    return "\n\n".join(block for block in blocks if block)
