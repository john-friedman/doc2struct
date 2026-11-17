"""Convert standardized rows to ParseBench's parse-output schema.

Conversion uses only parser output. References, test rules, subset names, and
expected answers never enter this module. Tables are HTML so the official
scorer receives the actual grid, without inventing header rows or cell spans.
"""
from html import escape

import doc2struct


PIPELINE = "doc2struct-tesseract-bb"


def paragraph(text):
    return "<p>" + escape(text).replace("\n", "<br>\n") + "</p>" if text else ""


def cell_html(text, runs):
    if not runs:
        return escape(text or "")
    parts = []
    for run in runs:
        tags = [tag for bit, tag in ((1, "strong"), (2, "em"), (4, "u"), (128, "s"))
                if run.get("flags", 0) & bit]
        parts.append("".join(f"<{tag}>" for tag in tags)
                     + escape(run.get("text", ""))
                     + "".join(f"</{tag}>" for tag in reversed(tags)))
    return "".join(parts)


def table_html(row, current_title):
    blocks = []
    if row.get("text") and row["text"] != current_title:
        blocks.append(paragraph(row["text"]))
    blocks.append(paragraph(row.get("preamble")))
    styles = {(cell["row"], cell["col"]): cell["runs"]
              for cell in row.get("cell_runs", [])}
    table = ["<table>", "<tbody>"]
    for r, cells in enumerate(row.get("matrix") or []):
        table.append("<tr>" + "".join(
            "<td>" + cell_html(value, styles.get((r, c))) + "</td>"
            for c, value in enumerate(cells)) + "</tr>")
    table.extend(["</tbody>", "</table>"])
    blocks.append("\n".join(table))
    for marker, text in row.get("footnotes") or []:
        blocks.append(paragraph(" ".join(part for part in (marker, text) if part)))
    blocks.append(paragraph(row.get("postamble")))
    return "\n\n".join(block for block in blocks if block)


def convert_to_parsebench(rows, example_id):
    chunks, pending = [], []
    current_title = None

    def flush():
        if pending:
            text = doc2struct.convert_standardized_text_to_markdown(pending, redundant="plain")
            if text.strip():
                chunks.append(text.strip())
            pending.clear()

    for row in rows:
        if row.get("type") == "title":
            current_title = row.get("text")
        if row.get("type") == "table":
            flush()
            chunks.append(table_html(row, current_title))
        else:
            pending.append(row)
    flush()
    markdown = "\n\n".join(chunks)
    # Standardized rows are document-level. The aggregate Markdown is carried
    # in one page entry, as in the existing SEC benchmark conversion.
    return {
        "task_type": "parse", "example_id": example_id, "pipeline_name": PIPELINE,
        "pages": [{"page_index": 0, "markdown": markdown}], "markdown": markdown,
    }
