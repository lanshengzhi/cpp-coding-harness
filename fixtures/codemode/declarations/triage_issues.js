// @options: {"max_output_tokens": 2000, "timeout_ms": 30000}
const issues = await tools.read({ path: "issues.json" });
return issues.length;
