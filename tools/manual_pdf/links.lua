-- Links in the manual that point into the repository (../README.md#the-baseband, THEMES.md, ../webserver/)
-- go to GitHub in the PDF, at the ref the PDF is built for; links inside the manual (#first-steps) and
-- web links stay as they are.
local repo = "https://github.com/Guru-RF/OpenX6X00"
local ref = os.getenv("MANUAL_REF") or "main"
local docs = os.getenv("MANUAL_DOCS") or "docs"

function Link(el)
  local t = el.target
  if t:match("^#") or t:match("^%a[%w+.-]*:") then
    return el
  end
  local kind = t:match("/$") and "tree" or "blob"
  if t:sub(1, 3) == "../" then
    el.target = repo .. "/" .. kind .. "/" .. ref .. "/" .. t:sub(4)
  else
    el.target = repo .. "/" .. kind .. "/" .. ref .. "/docs/" .. t
  end
  return el
end

-- A screenshot the manual names but the repository doesn't have is left out, not drawn as a broken image
function Image(el)
  if el.src:match("^%a[%w+.-]*:") then
    return el
  end
  local f = io.open(docs .. "/" .. el.src, "rb")
  if f then
    f:close()
    return el
  end
  io.stderr:write("manual_pdf: no " .. el.src .. ", left out\n")
  return {}
end
