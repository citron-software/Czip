"""Czip — gestionnaire d'archives ZIP avec interface française."""

from __future__ import annotations

import os
import shutil
import sys
import subprocess
import zipfile
from datetime import datetime
from pathlib import Path, PurePosixPath
import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog, ttk


APP_NAME = "Czip"
VERSION = "0.1.0"
SUPPORTED_FORMATS = ("*.zip", "*.7z", "*.tar", "*.wim", "*.gz", "*.lz4", "*.iso", "*.dll")
MODIFIABLE_FORMATS = {"zip", "7z", "tar", "wim"}


def find_7zip() -> str | None:
    """Find the bundled archive engine, falling back to a system installation."""
    bundle_dir = getattr(sys, "_MEIPASS", None)
    candidates = []
    if bundle_dir:
        candidates.extend((Path(bundle_dir) / "7z.exe", Path(bundle_dir) / "runtime" / "7z.exe"))
    candidates.append(Path(__file__).resolve().parent / "runtime" / "7z.exe")
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    for command in ("7z", "7zz", "7za"):
        found = shutil.which(command)
        if found:
            return found
    if sys.platform == "win32":
        for candidate in (
            Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "7-Zip" / "7z.exe",
            Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "7-Zip" / "7z.exe",
        ):
            if candidate.is_file():
                return str(candidate)
    return None


def run_7zip(*args: str, cwd: str | None = None) -> subprocess.CompletedProcess:
    executable = find_7zip()
    if not executable:
        raise FileNotFoundError("Le moteur d’archives intégré est introuvable. Vérifiez le contenu du paquet Czip.")
    return subprocess.run([executable, *args], cwd=cwd, capture_output=True, text=True, errors="replace", check=False)


def parse_7zip_listing(output: str) -> list[dict]:
    """Parse the stable key/value listing produced by `7z l -slt`."""
    records: list[dict] = []
    for block in output.split("\n\n"):
        fields = {}
        for line in block.splitlines():
            if " = " in line:
                key, value = line.split(" = ", 1)
                fields[key.strip()] = value.strip()
        name = fields.get("Path")
        if not name or "Size" not in fields:
            continue
        try:
            size = int(fields.get("Size", "0"))
            packed = int(fields.get("Packed Size", "0"))
        except ValueError:
            size, packed = 0, 0
        records.append({
            "name": name,
            "size": size,
            "packed": packed,
            "modified": fields.get("Modified", "—"),
            "is_dir": fields.get("Folder", "-") == "+" or fields.get("Attributes", "").startswith("D"),
            "encrypted": fields.get("Encrypted", "-") == "+",
        })
    return records


def human_size(size: int) -> str:
    value = float(size)
    for unit in ("o", "Ko", "Mo", "Go", "To"):
        if value < 1024 or unit == "To":
            return f"{value:.0f} {unit}" if unit == "o" else f"{value:.1f} {unit}"
        value /= 1024
    return f"{size} o"


class GlassButton(tk.Canvas):
    """A small, softly rounded action button drawn with native Tk canvas shapes."""
    def __init__(self, parent, text: str, command, accent: bool = False, width: int | None = None):
        self.label = text
        self.command = command
        self.accent = accent
        self.normal = "#5369e8" if accent else "#f7f9ff"
        self.hover = "#4358d7" if accent else "#e9efff"
        self.foreground = "#ffffff" if accent else "#263957"
        super().__init__(parent, width=width or max(88, len(text) * 8 + 28), height=40,
                         bg=parent.cget("bg"), highlightthickness=0, bd=0, cursor="hand2")
        self.bind("<Configure>", self._draw)
        self.bind("<Enter>", lambda _e: self._draw(hovering=True))
        self.bind("<Leave>", lambda _e: self._draw())
        self.bind("<Button-1>", self._activate)
        self._draw()

    def _draw(self, _event=None, hovering=False):
        self.delete("all")
        width = max(self.winfo_width(), int(self.cget("width")))
        height = max(self.winfo_height(), 40)
        color = self.hover if hovering else self.normal
        self.create_polygon(1, 14, 1, 12, 3, 6, 8, 2, 14, 1, width - 14, 1,
                            width - 8, 2, width - 3, 6, width - 1, 12, width - 1,
                            height - 12, width - 3, height - 6, width - 8, height - 2,
                            width - 14, height - 1, 14, height - 1, 8, height - 2,
                            3, height - 6, 1, height - 12, smooth=True, splinesteps=16,
                            fill=color, outline="#ffffff" if self.accent else "#ffffff")
        if self.accent:
            self.create_line(20, 3, width - 20, 3, fill="#91a0ff", width=1)
        self.create_text(width / 2, height / 2, text=self.label, fill=self.foreground,
                         font=("Segoe UI", 9, "bold" if self.accent else "normal"))

    def _activate(self, _event=None):
        self.command()


class CzipApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title(f"{APP_NAME} — Gestionnaire d'archives")
        self.geometry("1120x740")
        self.minsize(760, 480)
        self.configure(bg="#e9effc")
        self.archive_path: Path | None = None
        self.archive_password: str | None = None
        self.archive_encrypted_header = False
        self.archive_encrypted = False
        self.entries: list[dict] = []
        self._style()
        self._build_ui()
        self._bind_keys()
        self._refresh()

    def _style(self) -> None:
        style = ttk.Style(self)
        try:
            style.theme_use("vista" if sys.platform == "win32" else "clam")
        except tk.TclError:
            pass
        style.configure("Treeview", rowheight=35, font=("Segoe UI", 10), background="#f8faff", fieldbackground="#f8faff", borderwidth=0)
        style.configure("Treeview.Heading", font=("Segoe UI", 9, "bold"), foreground="#71819b", background="#edf2fb", padding=(10, 11), relief="flat")
        style.map("Treeview", background=[("selected", "#dce5ff")], foreground=[("selected", "#263b70")])
        style.configure("TLabel", font=("Segoe UI", 9))

    def _build_ui(self) -> None:
        self._make_menu()
        self.backdrop = tk.Canvas(self, bd=0, highlightthickness=0, bg="#e9effc")
        self.backdrop.place(x=0, y=0, relwidth=1, relheight=1)
        self.backdrop.bind("<Configure>", self._paint_backdrop)

        header = tk.Frame(self, bg="#eef2fb", height=80, padx=18)
        header.pack(fill="x", padx=22, pady=(18, 10))
        header.pack_propagate(False)
        logo = tk.Canvas(header, width=78, height=48, bd=0, highlightthickness=0, bg="#eef2fb")
        logo.pack(side="left", padx=(0, 10))
        logo.create_text(19, 24, text="C", fill="#25a85c", font=("Segoe UI", 36, "bold"))
        logo.create_polygon(45, 13, 49, 9, 69, 9, 73, 13, 73, 37, 69, 41, 49, 41, 45, 37,
                           smooth=True, splinesteps=12, fill="#d8f5e3", outline="#32a967", width=2)
        logo.create_line(49, 16, 69, 16, fill="#79ca98", width=2)
        logo.create_line(49, 22, 69, 22, fill="#79ca98", width=2)
        logo.create_line(49, 28, 69, 28, fill="#79ca98", width=2)
        logo.create_line(49, 34, 69, 34, fill="#79ca98", width=2)
        logo.create_line(57, 10, 57, 40, fill="#25a85c", width=2)
        logo.create_oval(55, 23, 59, 27, fill="#25a85c", outline="")
        titlebox = tk.Frame(header, bg="#eef2fb")
        titlebox.pack(side="left", pady=13)
        tk.Label(titlebox, text="Czip", font=("Segoe UI", 21, "bold"), fg="#1b2b49", bg="#eef2fb").pack(anchor="w")
        tk.Label(titlebox, text="ARCHIVE STUDIO   ·   ZIP  /  7Z  /  TAR  /  WIM", font=("Segoe UI", 8, "bold"), fg="#7786a2", bg="#eef2fb").pack(anchor="w", pady=(1, 0))
        self.archive_label = tk.Label(header, text="Aucune archive ouverte", font=("Segoe UI", 10, "bold"), fg="#465a7f", bg="#e5eafb", padx=16, pady=9, anchor="e")
        self.archive_label.pack(side="right", padx=(12, 0))

        toolbar = tk.Frame(self, bg="#f6f8fd", padx=12, pady=9, highlightbackground="#ffffff", highlightthickness=1)
        toolbar.pack(fill="x", padx=22, pady=(0, 9))
        self._tool_button(toolbar, "＋  Ajouter", self.add_files)
        self._tool_button(toolbar, "📦  Extraire", self.extract_archive, accent=True)
        self._tool_button(toolbar, "✓  Vérifier", self.test_archive)
        self._tool_button(toolbar, "−  Supprimer", self.delete_selected)
        tk.Frame(toolbar, bg="#d8e0f0", width=1, height=27).pack(side="left", padx=9)
        self._tool_button(toolbar, "＋  Nouvelle archive", self.new_archive)
        self._tool_button(toolbar, "📂  Ouvrir", self.open_archive)

        address = tk.Frame(self, bg="#eef2fb", padx=13, pady=9, highlightbackground="#ffffff", highlightthickness=1)
        address.pack(fill="x", padx=22, pady=(0, 9))
        tk.Label(address, text="⌖  EMPLACEMENT", bg="#eef2fb", fg="#71819b", font=("Segoe UI", 8, "bold")).pack(side="left", padx=(0, 12))
        self.location = tk.StringVar(value="Ouvrez une archive pour afficher son contenu")
        tk.Entry(address, textvariable=self.location, state="readonly", readonlybackground="#f9faff", relief="flat", bd=0, font=("Segoe UI", 9), fg="#445777").pack(side="left", fill="x", expand=True, ipady=8)

        body = tk.Frame(self, bg="#f8faff", padx=12, pady=8, highlightbackground="#ffffff", highlightthickness=1)
        body.pack(fill="both", expand=True, padx=22, pady=(0, 9))
        columns = ("name", "size", "compressed", "type", "modified")
        self.table = ttk.Treeview(body, columns=columns, show="headings", selectmode="extended")
        for col, label in (("name", "Nom"), ("size", "Taille"), ("compressed", "Compressé"), ("type", "Type"), ("modified", "Modifié le")):
            self.table.heading(col, text=label)
        self.table.column("name", width=440, minwidth=180, anchor="w")
        self.table.column("size", width=120, anchor="e")
        self.table.column("compressed", width=130, anchor="e")
        self.table.column("type", width=130, anchor="w")
        self.table.column("modified", width=160, anchor="w")
        self.table.tag_configure("folder", foreground="#5065d9", background="#f0f4ff")
        self.table.tag_configure("odd", background="#f3f6fc")
        scrollbar = ttk.Scrollbar(body, orient="vertical", command=self.table.yview)
        self.table.configure(yscrollcommand=scrollbar.set)
        self.table.pack(side="left", fill="both", expand=True, pady=8)
        scrollbar.pack(side="right", fill="y", pady=8)
        self.table.bind("<Double-1>", self._open_selected)
        self.table.bind("<Button-3>", self._context_menu)

        bottom = tk.Frame(self, bg="#edf1fa", padx=16, pady=8)
        bottom.pack(fill="x", side="bottom", padx=22, pady=(0, 13))
        self.status = tk.StringVar(value="Prêt")
        self.summary = tk.StringVar(value="")
        tk.Label(bottom, text="●", bg="#edf1fa", fg="#7d8dff", anchor="w", font=("Segoe UI", 8)).pack(side="left", padx=(0, 7))
        tk.Label(bottom, textvariable=self.status, bg="#edf1fa", fg="#465777", anchor="w", font=("Segoe UI", 9, "bold")).pack(side="left")
        tk.Label(bottom, textvariable=self.summary, bg="#edf1fa", fg="#7887a0", anchor="e", font=("Segoe UI", 9)).pack(side="right")

    def _paint_backdrop(self, event=None) -> None:
        if not hasattr(self, "backdrop"):
            return
        canvas = self.backdrop
        width = max(canvas.winfo_width(), 1)
        height = max(canvas.winfo_height(), 1)
        canvas.delete("all")
        left = (220, 236, 255)
        right = (244, 232, 255)
        steps = 96
        for i in range(steps):
            t = i / (steps - 1)
            color = "#%02x%02x%02x"%tuple(round(left[c] * (1 - t) + right[c] * t) for c in range(3))
            x0, x1 = int(width * i / steps), int(width * (i + 1) / steps) + 1
            canvas.create_rectangle(x0, 0, x1, height, fill=color, outline=color)
        canvas.create_oval(width * .58, -height * .55, width * 1.13, height * .52,
                           fill="#b9d9ff", outline="", stipple="gray50")
        canvas.create_oval(-width * .20, height * .48, width * .34, height * 1.18,
                           fill="#c8c2ff", outline="", stipple="gray50")
        canvas.create_oval(width * .60, height * .72, width * 1.06, height * 1.28,
                           fill="#f0c9f6", outline="", stipple="gray50")

    def _make_menu(self) -> None:
        menu = tk.Menu(self)
        file_menu = tk.Menu(menu, tearoff=False)
        file_menu.add_command(label="Nouvelle archive…", command=self.new_archive, accelerator="Ctrl+N")
        file_menu.add_command(label="Ouvrir…", command=self.open_archive, accelerator="Ctrl+O")
        file_menu.add_separator()
        file_menu.add_command(label="Quitter", command=self.destroy)
        menu.add_cascade(label="Fichier", menu=file_menu)
        action_menu = tk.Menu(menu, tearoff=False)
        action_menu.add_command(label="Ajouter des fichiers…", command=self.add_files, accelerator="Ctrl+A")
        action_menu.add_command(label="Extraire…", command=self.extract_archive, accelerator="Ctrl+E")
        action_menu.add_command(label="Vérifier l’archive", command=self.test_archive)
        action_menu.add_separator()
        action_menu.add_command(label="Supprimer la sélection", command=self.delete_selected, accelerator="Suppr")
        menu.add_cascade(label="Actions", menu=action_menu)
        help_menu = tk.Menu(menu, tearoff=False)
        help_menu.add_command(label="À propos de Czip", command=lambda: messagebox.showinfo("À propos", f"Czip {VERSION}\nGestionnaire d’archives ZIP simple et local."))
        menu.add_cascade(label="Aide", menu=help_menu)
        self.config(menu=menu)

    def _tool_button(self, parent, text, command, accent=False) -> None:
        widths = {"＋  Ajouter": 112, "📦  Extraire": 112, "✓  Vérifier": 105,
                  "−  Supprimer": 116, "＋  Nouvelle archive": 158, "📂  Ouvrir": 92}
        GlassButton(parent, text, command, accent=accent, width=widths.get(text)).pack(side="left", padx=4)

    def _bind_keys(self) -> None:
        self.bind("<Control-o>", lambda _e: self.open_archive())
        self.bind("<Control-n>", lambda _e: self.new_archive())
        self.bind("<Control-a>", lambda _e: self.add_files())
        self.bind("<Control-e>", lambda _e: self.extract_archive())
        self.bind("<Delete>", lambda _e: self.delete_selected())

    def _ready(self) -> bool:
        if self.archive_path is None:
            messagebox.showinfo("Aucune archive", "Ouvrez une archive prise en charge ou créez une archive ZIP.")
            return False
        return True

    def _refresh(self) -> None:
        for item in self.table.get_children():
            self.table.delete(item)
        self.entries.clear()
        if not self.archive_path:
            self.location.set("Ouvrez une archive pour afficher son contenu")
            self.archive_label.config(text="Aucune archive ouverte")
            self.status.set("Prêt")
            self.summary.set("")
            return
        try:
            self.archive_encrypted_header = False
            self.archive_encrypted = False
            if zipfile.is_zipfile(self.archive_path):
                with zipfile.ZipFile(self.archive_path) as archive:
                    records = [{"name": x.filename, "size": x.file_size, "packed": x.compress_size,
                                "modified": datetime(*x.date_time).strftime("%d/%m/%Y %H:%M"), "is_dir": x.is_dir(),
                                "encrypted": bool(x.flag_bits & 1)}
                               for x in archive.infolist()]
                self.archive_backend = "zip"
            else:
                result = self._list_with_password()
                if result.returncode != 0:
                    raise ValueError(result.stderr.strip() or result.stdout.strip() or "Format d’archive non reconnu par 7-Zip.")
                records = parse_7zip_listing(result.stdout)
                if not records and result.returncode != 0:
                    raise ValueError(result.stderr.strip() or "7-Zip n’a trouvé aucun élément dans ce fichier.")
                self.archive_backend = "7zip"
            self.entries = records
            self.archive_encrypted = any(item.get("encrypted", False) for item in records) or self.archive_encrypted_header
            total = sum(item["size"] for item in records)
            for i, item in enumerate(records):
                name = item["name"]
                is_dir = item["is_dir"]
                ext = "Dossier" if is_dir else (Path(name).suffix.upper().lstrip(".") or "Fichier")
                values = (f"📁  {name}" if is_dir else f"📄  {name}", "—" if is_dir else human_size(item["size"]), "—" if is_dir else human_size(item["packed"]), ext, item["modified"])
                self.table.insert("", "end", iid=str(i), values=values, tags=("folder" if is_dir else ("odd" if i % 2 else ""),))
            self.archive_label.config(text=self.archive_path.name)
            self.location.set(str(self.archive_path))
            self.status.set("Archive chargée")
            self.summary.set(f"{len(records)} élément(s)  ·  {human_size(total)} décompressé(s)  ·  {self.archive_backend.upper()}")
        except (OSError, ValueError, zipfile.BadZipFile) as exc:
            messagebox.showerror("Erreur de lecture", f"Impossible de lire cette archive.\n\n{exc}")
            self.archive_path = None
            self._refresh()

    def open_archive(self) -> None:
        name = filedialog.askopenfilename(title="Ouvrir une archive", filetypes=[("Archives prises en charge", " ".join(SUPPORTED_FORMATS)), ("Tous les fichiers", "*.*")])
        if name:
            self.archive_path = Path(name)
            self.archive_password = None
            self._refresh()

    def _ask_password(self, title: str = "Mot de passe de l’archive") -> str | None:
        return simpledialog.askstring(title, f"Mot de passe pour {self.archive_path.name} :", show="●", parent=self)

    def _list_with_password(self) -> subprocess.CompletedProcess:
        result = run_7zip("l", "-slt", str(self.archive_path))
        diagnostic = (result.stderr + "\n" + result.stdout).lower()
        if result.returncode and any(word in diagnostic for word in ("password", "encrypted", "mot de passe")):
            for _ in range(3):
                password = self.archive_password or self._ask_password("Archive protégée")
                if password is None:
                    return result
                self.archive_password = password
                self.archive_encrypted_header = True
                result = run_7zip("l", "-slt", f"-p{password}", str(self.archive_path))
                if result.returncode == 0:
                    break
                self.archive_password = None
        return result

    def new_archive(self) -> None:
        name = filedialog.asksaveasfilename(title="Créer une archive", defaultextension=".zip", filetypes=[("ZIP", "*.zip"), ("7Z", "*.7z")])
        if not name:
            return
        path = Path(name)
        if path.exists() and not messagebox.askyesno("Remplacer ?", f"Le fichier {path.name} existe déjà. Le remplacer ?"):
            return
        if path.suffix.lower() not in (".zip", ".7z"):
            messagebox.showerror("Format non pris en charge", "La création est disponible en ZIP et 7Z. Les autres formats ne permettent pas tous l’ajout de fichiers.")
            return
        files = filedialog.askopenfilenames(title="Choisir les fichiers à archiver")
        if not files:
            return
        password = None
        if messagebox.askyesno("Protéger l’archive ?", "Créer cette archive avec un mot de passe ?"):
            password = simpledialog.askstring("Mot de passe", "Saisissez un mot de passe :", show="●", parent=self)
            if not password:
                return
            confirmation = simpledialog.askstring("Confirmer le mot de passe", "Saisissez-le une seconde fois :", show="●", parent=self)
            if confirmation != password:
                messagebox.showerror("Mot de passe différent", "Les deux mots de passe ne correspondent pas.")
                return
        try:
            if path.suffix.lower() == ".zip" and not password:
                with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
                    for item in files:
                        source = Path(item)
                        archive.write(source, source.name)
            else:
                self._seven_zip_add(path, files, password=password, create=True)
            self.archive_path = path
            self.archive_password = password
            self._refresh()
            self.status.set("Nouvelle archive créée" + (" et chiffrée" if password else ""))
        except (OSError, ValueError) as exc:
            messagebox.showerror("Erreur", f"Impossible de créer l’archive.\n\n{exc}")

    def _seven_zip_add(self, archive_path: Path, files: tuple | list, password: str | None = None, create: bool = False) -> None:
        kind = archive_path.suffix.lower().lstrip(".")
        base = ["a", "-t" + kind, str(archive_path)]
        if password:
            base.append(f"-p{password}")
            if kind == "7z":
                base.append("-mhe=on")
            elif kind == "zip":
                base.append("-mem=AES256")
        for index, item in enumerate(files):
            source = Path(item)
            result = run_7zip(*base, "-y", "--", source.name, cwd=str(source.parent))
            if result.returncode != 0:
                detail = result.stderr.strip() or result.stdout.strip() or "7-Zip n’a pas pu modifier ce format."
                if not create and any(word in detail.lower() for word in ("password", "encrypted", "mot de passe")):
                    self.archive_password = None
                    password = self._ask_password()
                    if password:
                        self.archive_password = password
                        return self._seven_zip_add(archive_path, files[index:], password=password)
                raise ValueError(f"{source.name} : {detail}")

    def add_files(self) -> None:
        if not self._ready():
            return
        if self.archive_path.suffix.lower().lstrip(".") not in MODIFIABLE_FORMATS:
            messagebox.showinfo("Format non modifiable", "Ce format ne permet pas d’ajouter des fichiers de façon fiable. Créez une archive ZIP ou 7Z pour regrouper ces fichiers.")
            return
        paths = filedialog.askopenfilenames(title="Ajouter des fichiers")
        if not paths:
            return
        try:
            if self.archive_backend == "zip" and not self.archive_encrypted:
                with zipfile.ZipFile(self.archive_path, "a", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
                    for name in paths:
                        path = Path(name)
                        archive.write(path, path.name)
            else:
                self._seven_zip_add(self.archive_path, paths, password=self.archive_password)
            self._refresh()
            self.status.set(f"{len(paths)} fichier(s) ajouté(s)")
        except (OSError, ValueError, zipfile.BadZipFile, RuntimeError) as exc:
            messagebox.showerror("Erreur d’ajout", f"Impossible d’ajouter les fichiers.\n\n{exc}")

    def _selected_names(self) -> list[str]:
        return [self.entries[int(iid)]["name"] for iid in self.table.selection() if 0 <= int(iid) < len(self.entries)]

    def extract_archive(self) -> None:
        if not self._ready():
            return
        names = self._selected_names()
        target = filedialog.askdirectory(title="Choisir le dossier de destination")
        if not target:
            return
        try:
            root = Path(target).resolve()
            if self.archive_backend == "zip" and not self.archive_encrypted:
                with zipfile.ZipFile(self.archive_path) as archive:
                    chosen = names or archive.namelist()
                    safe = []
                    for name in chosen:
                        dest = (root / Path(*PurePosixPath(name).parts)).resolve()
                        if dest != root and root not in dest.parents:
                            raise ValueError(f"Chemin d’extraction non autorisé : {name}")
                        safe.append(name)
                    for name in safe:
                        archive.extract(name, root)
            else:
                args = ["x", str(self.archive_path), f"-o{root}", "-y"]
                if names:
                    args.extend(["--", *names])
                password = self.archive_password
                if self.archive_encrypted and not password:
                    password = self._ask_password()
                    if not password:
                        return
                    self.archive_password = password
                if password:
                    args.insert(1, f"-p{password}")
                result = run_7zip(*args)
                if result.returncode != 0 and not password and any(word in (result.stderr + result.stdout).lower() for word in ("password", "encrypted")):
                    password = self._ask_password()
                    if not password:
                        return
                    self.archive_password = password
                    args.insert(1, f"-p{password}")
                    result = run_7zip(*args)
                if result.returncode != 0:
                    if self.archive_encrypted:
                        self.archive_password = None
                    raise ValueError(result.stderr.strip() or result.stdout.strip() or "Échec de l’extraction.")
                safe = names or [item["name"] for item in self.entries]
            self.status.set(f"Extraction terminée dans {root}")
            messagebox.showinfo("Extraction terminée", f"{len(safe)} élément(s) extrait(s) dans :\n{root}")
        except (OSError, ValueError, zipfile.BadZipFile, RuntimeError) as exc:
            messagebox.showerror("Erreur d’extraction", f"L’extraction a échoué.\n\n{exc}")

    def delete_selected(self) -> None:
        if not self._ready():
            return
        if self.archive_path.suffix.lower().lstrip(".") not in MODIFIABLE_FORMATS:
            messagebox.showinfo("Format non modifiable", "Ce format ne permet pas de supprimer des éléments de façon fiable. Vous pouvez extraire son contenu puis créer une archive ZIP ou 7Z.")
            return
        names = self._selected_names()
        if not names:
            messagebox.showinfo("Sélection vide", "Sélectionnez un ou plusieurs éléments à supprimer.")
            return
        if not messagebox.askyesno("Confirmer la suppression", f"Supprimer {len(names)} élément(s) de l’archive ?"):
            return
        try:
            temporary = self.archive_path.with_name(self.archive_path.name + ".czip-tmp")
            if getattr(self, "archive_backend", "zip") != "zip" or self.archive_encrypted:
                password = self.archive_password
                if self.archive_encrypted and not password:
                    password = self._ask_password()
                    if not password:
                        return
                    self.archive_password = password
                args = ["d", str(self.archive_path)]
                if password:
                    args.append(f"-p{password}")
                result = run_7zip(*args, "--", *names)
                if result.returncode != 0:
                    if self.archive_encrypted:
                        self.archive_password = None
                    raise ValueError(result.stderr.strip() or result.stdout.strip() or "Échec de la suppression.")
                self._refresh()
                self.status.set(f"{len(names)} élément(s) supprimé(s)")
                return
            with zipfile.ZipFile(self.archive_path, "r") as source, zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED) as dest:
                for info in source.infolist():
                    if info.filename not in names:
                        dest.writestr(info, source.read(info.filename))
            os.replace(temporary, self.archive_path)
            self._refresh()
            self.status.set(f"{len(names)} élément(s) supprimé(s)")
        except (OSError, ValueError, zipfile.BadZipFile, RuntimeError) as exc:
            try:
                temporary.unlink(missing_ok=True)
            except (OSError, UnboundLocalError):
                pass
            messagebox.showerror("Erreur", f"Impossible de modifier l’archive.\n\n{exc}")

    def test_archive(self) -> None:
        if not self._ready():
            return
        try:
            if getattr(self, "archive_backend", "zip") == "zip" and not self.archive_encrypted:
                with zipfile.ZipFile(self.archive_path) as archive:
                    bad = archive.testzip()
                output = bad
            else:
                password = self.archive_password
                if self.archive_encrypted and not password:
                    password = self._ask_password()
                    if not password:
                        return
                    self.archive_password = password
                args = ["t", str(self.archive_path)]
                if password:
                    args.insert(1, f"-p{password}")
                result = run_7zip(*args)
                output = result.stderr.strip() or result.stdout.strip() if result.returncode != 0 else None
                if output and self.archive_encrypted:
                    self.archive_password = None
            if output:
                self.status.set("Vérification échouée")
                detail = f"Une erreur CRC a été détectée dans :\n{output}" if self.archive_backend == "zip" else f"7-Zip a signalé un problème :\n{output}"
                messagebox.showerror("Archive endommagée", detail)
            else:
                self.status.set("Vérification réussie")
                messagebox.showinfo("Vérification réussie", "Tous les fichiers de l’archive sont intacts.")
        except (OSError, zipfile.BadZipFile, RuntimeError) as exc:
            messagebox.showerror("Erreur de vérification", f"Impossible de vérifier l’archive.\n\n{exc}")

    def _open_selected(self, _event=None) -> None:
        selection = self.table.selection()
        if selection:
            index = int(selection[0])
            item = self.entries[index]
            name = item["name"]
            if item["is_dir"]:
                self.status.set(f"Dossier : {name}")
            else:
                self.extract_archive()

    def _context_menu(self, event) -> None:
        row = self.table.identify_row(event.y)
        if row and row not in self.table.selection():
            self.table.selection_set(row)
        menu = tk.Menu(self, tearoff=False)
        menu.add_command(label="Extraire la sélection…", command=self.extract_archive)
        menu.add_command(label="Supprimer de l’archive", command=self.delete_selected)
        menu.tk_popup(event.x_root, event.y_root)


if __name__ == "__main__":
    CzipApp().mainloop()
