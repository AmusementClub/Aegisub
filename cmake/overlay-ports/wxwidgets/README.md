# wxwidgets overlay port

This port is a snapshot of vcpkg's built-in `wxwidgets` port at the repository
baseline in `vcpkg.json`:

`58950f88544e4637524dbd6a01d0317cf4cb77fc`

It exists so Aegisub can carry narrowly scoped wxSTC fixes without modifying a
developer's global vcpkg checkout. Keep local changes as small patches listed
in `portfile.cmake`.
