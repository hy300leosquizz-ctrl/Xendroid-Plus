package xendroid.compose.ui.library

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import android.os.Environment
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import xendroid.compose.R
import xendroid.compose.core.AllFilesAccess
import xendroid.compose.data.FolderGames
import xendroid.compose.data.Game
import xendroid.compose.data.StandardFolders
import xendroid.compose.ui.content.ContentFolders
import xendroid.compose.ui.design.BadgeTone
import xendroid.compose.ui.design.LocalXdToast
import xendroid.compose.ui.design.Xd
import xendroid.compose.ui.design.XdBadge
import xendroid.compose.ui.design.XdButton
import xendroid.compose.ui.design.XdButtonKind
import xendroid.compose.ui.design.XdButtonSize
import xendroid.compose.ui.design.XdCard
import xendroid.compose.ui.design.XdEmpty
import xendroid.compose.ui.design.XdIcons
import xendroid.compose.ui.design.XdNote
import xendroid.compose.ui.design.XdSingleScreen
import xendroid.compose.ui.design.XdText

/**
 * L03, lote 6: the folders the library looks in, as a screen: how many games each holds, which
 * receives full-game installs (the first; "Install here" moves another to the top), which are not
 * available now; add one through the folder browser, look again. Removing one only stops looking
 * in it: no file is moved or deleted.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun GameFoldersScreen(
    folders: List<String>,
    unavailable: List<String>,
    games: List<Game>,
    scanning: Boolean,
    onAdd: () -> Unit,
    onRemove: (String) -> Unit,
    onMakeInstallFolder: (String) -> Unit,
    onRescan: () -> Unit,
    onBack: () -> Unit,
    /** Round 2: makes XenDroid/<games>, /TU and /DLC and adds them; null where it cannot. */
    onCreateStandard: (() -> Unit)? = null,
    /** The standard games folder, to tell whether it is in the list already. */
    standardGames: String? = null,
    /** Replaces an existing folder in place through the folder browser. */
    onChange: ((String) -> Unit)? = null,
) {
    BackHandler(onBack = onBack)
    val c = Xd.colors
    // The library's games under each folder (its own walk found them); a folder inside another
    // is walked once, so its games count for both here.
    val counts = folders.associateWith { folder ->
        val root = folder.trimEnd('/') + "/"
        games.count { it.launchUri.startsWith(root) }
    }
    XdSingleScreen(
        title = stringResource(R.string.lib_menu_folders),
        subtitle = pluralStringResource(R.plurals.xd_fd_folders, folders.size, folders.size),
        onBack = onBack,
        headIcon = XdIcons.folder,
        actions = { XdButton(stringResource(R.string.folders_add), onAdd, kind = XdButtonKind.PRIMARY, size = XdButtonSize.SM, icon = XdIcons.plus) },
    ) {
        Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                val total = games.size
                Text(stringResource(R.string.xd_fd_summary, gamesText(total), pluralStringResource(R.plurals.xd_fd_folders, folders.size, folders.size))
                    .replaceFirstChar { it.uppercase() }, style = XdText.note, color = c.fg3, modifier = Modifier.weight(1f))
                XdButton(stringResource(if (scanning) R.string.xd_fd_scanning else R.string.xd_cm_look_again), onRescan,
                    kind = XdButtonKind.GHOST, size = XdButtonSize.SM, icon = XdIcons.refresh, enabled = !scanning)
            }
            if (folders.isEmpty()) XdEmpty(stringResource(R.string.folders_none)) {
                XdButton(stringResource(R.string.folders_add), onAdd, kind = XdButtonKind.PRIMARY, size = XdButtonSize.SM, icon = XdIcons.plus)
            }
            else BoxWithConstraints {
                val narrow = maxWidth < 560.dp
                XdCard(Modifier.fillMaxWidth()) {
                    Column {
                        folders.forEachIndexed { i, folder ->
                            FolderRow(folder, install = i == 0, away = folder in unavailable, games = counts[folder] ?: 0, narrow = narrow,
                                onChange = onChange?.let { { it(folder) } },
                                onRemove = { onRemove(folder) }, onInstallHere = { onMakeInstallFolder(folder) })
                            if (i < folders.lastIndex && !c.controller) HorizontalDivider(thickness = 1.dp, color = c.line)
                        }
                    }
                }
            }
            if (onCreateStandard != null && standardGames != null && folders.none { it.trimEnd('/') == standardGames }) {
                XdCard(Modifier.fillMaxWidth(), title = stringResource(R.string.xd_fd_std), icon = XdIcons.folder) {
                    Text(stringResource(R.string.xd_fd_std_note, File(standardGames).name), style = XdText.note, color = c.fg3)
                    XdButton(stringResource(R.string.xd_fd_std), onCreateStandard, size = XdButtonSize.SM, icon = XdIcons.plus)
                }
            }
            XdNote(stringResource(R.string.folders_note))
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun FolderRow(
    path: String,
    install: Boolean,
    away: Boolean,
    games: Int,
    narrow: Boolean,
    onChange: (() -> Unit)? = null,
    onRemove: () -> Unit,
    onInstallHere: () -> Unit,
) {
    val c = Xd.colors
    val actions: @Composable () -> Unit = {
        if (onChange != null) XdButton(stringResource(R.string.xd_fd_change), onChange, kind = XdButtonKind.GHOST, size = XdButtonSize.SM)
        if (!install && !away) XdButton(stringResource(R.string.xd_fd_install_here), onInstallHere, kind = XdButtonKind.GHOST, size = XdButtonSize.SM)
        XdButton(stringResource(R.string.common_remove), onRemove, kind = XdButtonKind.GHOST, size = XdButtonSize.SM)
    }
    Row(Modifier.fillMaxWidth().padding(vertical = 12.dp), verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Icon(XdIcons.folder, null, Modifier.size(24.dp), tint = if (away) c.warn else c.fg3)
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(displayPath(path), style = XdText.label, color = if (away) c.fg2 else c.fg)
            if (install || away) FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                if (install) XdBadge(stringResource(R.string.folders_installs_here), tone = BadgeTone.ACCENT)
                if (away) XdBadge(stringResource(R.string.folders_unavailable), tone = BadgeTone.WARN)
            }
            Text(if (away) stringResource(R.string.xd_fd_unavailable_detail) else gamesText(games).replaceFirstChar { it.uppercase() },
                style = XdText.small, color = c.fg3)
            if (narrow) FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) { actions() }
        }
        if (!narrow) Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) { actions() }
    }
}

/** "3 jogos", or "nenhum jogo": Portuguese plural rules put 0 with 1 ("0 jogo"). */
@Composable
fun gamesText(n: Int): String = if (n == 0) stringResource(R.string.xd_fd_no_games) else pluralStringResource(R.plurals.xd_cm_games, n, n)

/** How many games [dir] holds, counted in the background (null while counting). */
@Composable
fun folderGameCount(dir: File): FolderGames.Count? {
    val count by produceState<FolderGames.Count?>(null, dir.absolutePath) {
        value = withContext(Dispatchers.IO) { runCatching { FolderGames.count(dir) }.getOrNull() }
    }
    return count
}

/** Readable directory near [oldPath] to open the browser at when replacing an existing folder. */
internal fun folderBrowserStartFor(oldPath: String): File? {
    val old = File(oldPath)
    val parent = old.parentFile
    if (parent != null && parent.isDirectory && parent.listFiles() != null) return parent
    return generateSequence(old) { it.parentFile }.firstOrNull { it.isDirectory && it.listFiles() != null }
}

/** Self-contained Game Folders destination shared by the library, Start menu, and Settings. */
@Composable
fun ManagedGameFoldersScreen(viewModel: GameLibraryViewModel, onBack: () -> Unit) {
    val context = LocalContext.current
    val toast = LocalXdToast.current
    val state by viewModel.state.collectAsStateWithLifecycle()
    val folders by viewModel.folders.collectAsStateWithLifecycle()
    val scanProgress by viewModel.scanProgress.collectAsStateWithLifecycle()
    val isRefreshing by viewModel.isRefreshing.collectAsStateWithLifecycle()
    val storageRoots = rememberStorageRoots()
    var adding by rememberSaveable { mutableStateOf(false) }
    var changing by rememberSaveable { mutableStateOf<String?>(null) }

    LaunchedEffect(Unit) { viewModel.loadFolders() }

    if (adding) {
        FolderBrowserScreen(
            onFolderChosen = { path -> adding = false; viewModel.onRealPathFolderPicked(path) },
            onCancel = { adding = false },
        )
        return
    }
    changing?.let { oldPath ->
        FolderBrowserScreen(
            onFolderChosen = { newPath ->
                val before = folders
                changing = null
                if (newPath != oldPath) {
                    viewModel.replaceFolder(oldPath, newPath) {
                        toast.show(
                            context.getString(
                                R.string.xd_fd_changed,
                                StoragePaths.display(newPath, storageRoots),
                                StoragePaths.display(oldPath, storageRoots),
                            ),
                            context.getString(R.string.xd_undo),
                        ) { viewModel.restoreFolders(before) }
                    }
                }
            },
            onCancel = { changing = null },
            start = folderBrowserStartFor(oldPath),
        )
        return
    }

    val createStandard: (() -> Unit)? = if (!AllFilesAccess.isSupported) null else ({
        if (!AllFilesAccess.isGranted()) AllFilesAccess.requestAccess(context)
        else {
            val gamesName = context.getString(R.string.xd_fd_std_games)
            val made = StandardFolders.create(Environment.getExternalStorageDirectory(), gamesName)
            if (made == null) toast.show(context.getString(R.string.xd_fd_std_failed))
            else {
                viewModel.onRealPathFolderPicked(made.games.absolutePath)
                ContentFolders.add(context, made.updates.absolutePath)
                ContentFolders.add(context, made.dlc.absolutePath)
                toast.show(context.getString(R.string.xd_fd_std_done, gamesName))
            }
        }
    })

    GameFoldersScreen(
        folders = folders,
        unavailable = (state as? LibraryUiState.Loaded)?.unavailableRoots.orEmpty(),
        games = (state as? LibraryUiState.Loaded)?.games.orEmpty(),
        scanning = scanProgress != null || isRefreshing,
        onAdd = {
            if (AllFilesAccess.isGranted()) adding = true
            else AllFilesAccess.requestAccess(context)
        },
        onChange = { folder ->
            if (AllFilesAccess.isGranted()) changing = folder
            else AllFilesAccess.requestAccess(context)
        },
        onCreateStandard = createStandard,
        standardGames = remember {
            StandardFolders.under(Environment.getExternalStorageDirectory(), context.getString(R.string.xd_fd_std_games)).games.absolutePath
        },
        onRemove = { folder ->
            val before = folders
            viewModel.removeFolder(folder)
            toast.show(
                context.getString(R.string.xd_fd_removed, StoragePaths.display(folder, storageRoots)),
                context.getString(R.string.xd_undo),
            ) { viewModel.restoreFolders(before) }
        },
        onMakeInstallFolder = { folder ->
            viewModel.makeInstallFolder(folder)
            toast.show(context.getString(R.string.xd_fd_install_moved))
        },
        onRescan = { viewModel.refresh() },
        onBack = onBack,
    )
}
