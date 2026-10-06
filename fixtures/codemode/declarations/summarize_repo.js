const listing = await tools.read({ path: "README.md" });
return listing.slice(0, 200);
