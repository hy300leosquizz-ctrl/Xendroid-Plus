package xendroid.compose.ui.patches

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Clear
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import xendroid.compose.R
import xendroid.compose.patches.AspectRatioPatchHelper
import xendroid.compose.patches.AspectRatioPatchHelper.QuickFilter
import xendroid.compose.patches.GamePatchesViewModel
import xendroid.compose.patches.PatchEntry
import xendroid.compose.patches.PatchFile

/**
 * Lists the bundled and user patches for one game, grouped by file, with instant search,
 * quick filter chips (All / Enabled / 60 FPS / Widescreen), and a built-in 20:9 & 21:9
 * ultrawide aspect-ratio patch generator & IEEE-754 `be32` helper.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun GamePatchesScreen(
    vm: GamePatchesViewModel,
    gameName: String,
    onBack: () -> Unit,
) {
    val state by vm.state.collectAsStateWithLifecycle()
    val message by vm.message.collectAsStateWithLifecycle()
    val pickFile = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) vm.importUserPatch(uri)
    }
    var removing by remember { mutableStateOf<PatchFile?>(null) }
    var aspectDialogOpen by rememberSaveable { mutableStateOf(false) }

    message?.let { text ->
        AlertDialog(
            onDismissRequest = vm::clearMessage,
            text = { Text(text) },
            confirmButton = { TextButton(onClick = vm::clearMessage) { Text(stringResource(R.string.common_ok)) } },
        )
    }
    removing?.let { file ->
        AlertDialog(
            onDismissRequest = { removing = null },
            title = { Text(stringResource(R.string.pt_remove_title, file.variantLabel)) },
            text = { Text(stringResource(R.string.pt_remove_note)) },
            confirmButton = {
                TextButton(onClick = { removing = null; vm.removeUserPatch(file) }) {
                    Text(stringResource(R.string.common_remove))
                }
            },
            dismissButton = { TextButton(onClick = { removing = null }) { Text(stringResource(R.string.common_cancel)) } },
        )
    }
    if (aspectDialogOpen) {
        val hasTemplate = (state as? GamePatchesViewModel.UiState.Loaded)?.hasAspectTemplate == true
        AspectRatioHelperDialog(
            hasTemplate = hasTemplate,
            onDismiss = { aspectDialogOpen = false },
            onGenerate = { ratio, label, customAddress ->
                aspectDialogOpen = false
                vm.createUltrawidePatch(gameName, ratio, label, customAddress)
            },
        )
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Text(
                        if (gameName.isNotBlank()) stringResource(R.string.pt_title_game, gameName) else stringResource(R.string.pt_title),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.common_back))
                    }
                },
                actions = {
                    TextButton(onClick = { aspectDialogOpen = true }) {
                        Text(stringResource(R.string.pt_aspect_btn))
                    }
                    IconButton(onClick = { pickFile.launch(arrayOf("*/*")) }) {
                        Icon(Icons.Default.Add, contentDescription = stringResource(R.string.pt_add))
                    }
                },
            )
        },
    ) { padding ->
        Box(Modifier.fillMaxSize().padding(padding), contentAlignment = Alignment.Center) {
            when (val s = state) {
                GamePatchesViewModel.UiState.Loading -> CircularProgressIndicator()
                GamePatchesViewModel.UiState.Empty ->
                    Column(
                        horizontalAlignment = Alignment.CenterHorizontally,
                        verticalArrangement = Arrangement.spacedBy(12.dp),
                        modifier = Modifier.padding(24.dp),
                    ) {
                        Text(stringResource(R.string.pt_empty), style = MaterialTheme.typography.bodyLarge)
                        OutlinedButton(onClick = { aspectDialogOpen = true }) {
                            Text(stringResource(R.string.pt_aspect_title))
                        }
                    }
                is GamePatchesViewModel.UiState.Error ->
                    Text(s.message, modifier = Modifier.padding(24.dp))
                is GamePatchesViewModel.UiState.Loaded ->
                    PatchList(
                        files = s.files,
                        conflicts = s.conflicts,
                        versions = s.versions,
                        versionKnown = s.versionKnown,
                        onToggle = vm::toggle,
                        onUpdate = vm::update,
                        onRemove = { removing = it },
                    )
            }
        }
    }
}

@Composable
private fun PatchList(
    files: List<PatchFile>,
    conflicts: List<xendroid.compose.patches.PatchConflict>,
    versions: Map<String, xendroid.compose.patches.PatchVersion.Match>,
    versionKnown: Boolean,
    onToggle: (PatchFile, PatchEntry, Boolean) -> Unit,
    onUpdate: (PatchFile, GamePatchesViewModel.UpdateAction) -> Unit,
    onRemove: (PatchFile) -> Unit,
) {
    var query by rememberSaveable { mutableStateOf("") }
    var filter by rememberSaveable { mutableStateOf(QuickFilter.ALL) }
    val showHeaders = files.size > 1
    val filteredFiles = remember(files, query, filter) {
        files.mapNotNull { file ->
            val matching = file.entries.filter { AspectRatioPatchHelper.matches(it, file.variantLabel, query, filter) }
            if (matching.isEmpty()) null else file to matching
        }
    }

    LazyColumn(Modifier.fillMaxSize()) {
        item(key = "search-bar") {
            Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp)) {
                OutlinedTextField(
                    value = query,
                    onValueChange = { query = it },
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    placeholder = { Text(stringResource(R.string.pt_search_hint)) },
                    leadingIcon = { Icon(Icons.Default.Search, contentDescription = null) },
                    trailingIcon = {
                        if (query.isNotEmpty()) {
                            IconButton(onClick = { query = "" }) {
                                Icon(Icons.Default.Clear, contentDescription = stringResource(R.string.common_cancel))
                            }
                        }
                    },
                )
                Row(
                    Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(top = 8.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    val chips = listOf(
                        QuickFilter.ALL to stringResource(R.string.xd_set_filter_all),
                        QuickFilter.ENABLED to stringResource(R.string.pt_filter_enabled),
                        QuickFilter.FPS_60 to "60 FPS",
                        QuickFilter.WIDESCREEN to stringResource(R.string.pt_filter_widescreen),
                    )
                    for ((kind, label) in chips) {
                        FilterChip(
                            selected = filter == kind,
                            onClick = { filter = kind },
                            label = { Text(label) },
                        )
                    }
                }
            }
        }
        item(key = "note") {
            Text(
                stringResource(R.string.pt_note),
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
            )
        }
        if (!versionKnown) {
            item(key = "version-unknown") {
                Text(
                    stringResource(R.string.pt_version_unknown),
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
                )
            }
        }
        if (conflicts.isNotEmpty()) {
            item(key = "conflicts") {
                OutlinedCard(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp)) {
                    Text(
                        stringResource(R.string.pt_conflicts) + "\n" + conflicts.map {
                            stringResource(R.string.pt_conflict_line, it.first, it.second, "0x%08X".format(it.address))
                        }.joinToString("\n"),
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.padding(12.dp),
                    )
                }
            }
        }
        if (filteredFiles.isEmpty()) {
            item(key = "no-search-results") {
                Text(
                    stringResource(R.string.pt_search_empty),
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 20.dp),
                )
            }
        }
        for ((file, entries) in filteredFiles) {
            if (showHeaders || file.mine) {
                item(key = "hdr:${file.fileName}") {
                    Row(
                        Modifier.fillMaxWidth().padding(start = 16.dp, top = 12.dp, bottom = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            if (file.mine) stringResource(R.string.pt_added_by_you, file.variantLabel) else file.variantLabel,
                            style = MaterialTheme.typography.titleSmall,
                            modifier = Modifier.weight(1f),
                        )
                        if (file.mine) TextButton(onClick = { onRemove(file) }) { Text(stringResource(R.string.common_remove)) }
                    }
                }
            }
            when (versions[file.fileName]) {
                xendroid.compose.patches.PatchVersion.Match.YOURS -> item(key = "ver:${file.fileName}") {
                    VersionLine(stringResource(R.string.pt_version_yours), MaterialTheme.colorScheme.primary)
                }
                xendroid.compose.patches.PatchVersion.Match.OTHER -> item(key = "ver:${file.fileName}") {
                    VersionLine(stringResource(R.string.pt_version_other), MaterialTheme.colorScheme.error)
                }
                else -> {}
            }
            file.update?.let { update ->
                item(key = "upd:${file.fileName}") { CatalogUpdateCard(update) { onUpdate(file, it) } }
            }
            items(entries, key = { "${file.fileName}#${it.index}" }) { entry ->
                PatchRow(
                    entry = entry,
                    onCheckedChange = { checked -> onToggle(file, entry, checked) },
                )
            }
            item(key = "div:${file.fileName}") { HorizontalDivider() }
        }
    }
}

@Composable
private fun AspectRatioHelperDialog(
    hasTemplate: Boolean,
    onDismiss: () -> Unit,
    onGenerate: (Float, String, String?) -> Unit,
) {
    val context = LocalContext.current
    val metrics = context.resources.displayMetrics
    val screenW = maxOf(metrics.widthPixels, metrics.heightPixels).coerceAtLeast(1)
    val screenH = minOf(metrics.widthPixels, metrics.heightPixels).coerceAtLeast(1)
    val nativeRatio = (screenW.toFloat() / screenH.toFloat()).coerceIn(1.33f, 3.5f)
    var selectedPreset by remember { mutableStateOf<AspectRatioPatchHelper.Preset?>(AspectRatioPatchHelper.Preset.RATIO_20_9) }
    var customAddr by rememberSaveable { mutableStateOf("") }

    val activeRatio = selectedPreset?.ratio ?: nativeRatio
    val activeLabel = selectedPreset?.label ?: "${screenW}x${screenH}"
    val activeHex = remember(activeRatio) { runCatching { AspectRatioPatchHelper.floatToBe32Hex(activeRatio) }.getOrDefault("0x400E38E4") }
    val canCreate = hasTemplate || customAddr.trim().isNotEmpty()

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.pt_aspect_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text(stringResource(R.string.pt_aspect_desc), style = MaterialTheme.typography.bodySmall)
                Row(
                    Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    for (preset in AspectRatioPatchHelper.Preset.entries) {
                        FilterChip(
                            selected = selectedPreset == preset,
                            onClick = { selectedPreset = preset },
                            label = { Text(preset.label) },
                        )
                    }
                    FilterChip(
                        selected = selectedPreset == null,
                        onClick = { selectedPreset = null },
                        label = { Text(stringResource(R.string.pt_aspect_screen)) },
                    )
                }
                OutlinedCard(Modifier.fillMaxWidth()) {
                    Row(
                        Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 8.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.SpaceBetween,
                    ) {
                        Text(
                            "$activeLabel · ${"%.4f".format(activeRatio)} · $activeHex",
                            style = MaterialTheme.typography.bodyMedium,
                            modifier = Modifier.weight(1f),
                        )
                        TextButton(onClick = {
                            (context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager)
                                ?.setPrimaryClip(ClipData.newPlainText("be32", activeHex))
                        }) {
                            Text(stringResource(R.string.xd_copy))
                        }
                    }
                }
                if (!hasTemplate) {
                    OutlinedTextField(
                        value = customAddr,
                        onValueChange = { customAddr = it.take(18) },
                        singleLine = true,
                        label = { Text(stringResource(R.string.pt_aspect_addr_label)) },
                        placeholder = { Text("0x82012340") },
                        modifier = Modifier.fillMaxWidth(),
                    )
                }
            }
        },
        confirmButton = {
            TextButton(
                enabled = canCreate,
                onClick = { onGenerate(activeRatio, activeLabel, customAddr.trim().ifEmpty { null }) },
            ) {
                Text(stringResource(R.string.pt_aspect_create))
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.common_cancel)) }
        },
    )
}

@Composable
private fun VersionLine(text: String, color: androidx.compose.ui.graphics.Color) {
    Text(text, style = MaterialTheme.typography.bodySmall, color = color,
        modifier = Modifier.padding(horizontal = 16.dp, vertical = 2.dp))
}

/** L10: news about this file's bundled catalog: applied by itself (undo) or waiting (preview). */
@Composable
private fun CatalogUpdateCard(update: xendroid.compose.patches.PatchUpdate, onAction: (GamePatchesViewModel.UpdateAction) -> Unit) {
    val gone = update.dropped.takeIf { it.isNotEmpty() }?.let { " " + stringResource(R.string.pt_dropped, it.joinToString(", ")) } ?: ""
    OutlinedCard(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 6.dp)) {
        Column(Modifier.padding(12.dp)) {
            Text(
                if (update.pending) {
                    stringResource(R.string.pt_update_pending, update.keptOn.size) + gone
                } else {
                    stringResource(R.string.pt_updated, update.keptOn.size) + gone
                },
                style = MaterialTheme.typography.bodySmall,
            )
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                if (update.pending) {
                    TextButton(onClick = { onAction(GamePatchesViewModel.UpdateAction.KEEP_MINE) }) { Text(stringResource(R.string.pt_keep_mine)) }
                    TextButton(onClick = { onAction(GamePatchesViewModel.UpdateAction.APPLY) }) { Text(stringResource(R.string.pt_update)) }
                } else {
                    if (update.canUndo) {
                        TextButton(onClick = { onAction(GamePatchesViewModel.UpdateAction.UNDO) }) { Text(stringResource(R.string.pt_undo)) }
                    }
                    TextButton(onClick = { onAction(GamePatchesViewModel.UpdateAction.DISMISS) }) { Text(stringResource(R.string.common_ok)) }
                }
            }
        }
    }
}

@Composable
private fun PatchRow(entry: PatchEntry, onCheckedChange: (Boolean) -> Unit) {
    ListItem(
        headlineContent = { Text(entry.name) },
        supportingContent = {
            val sub = listOfNotNull(
                entry.desc?.takeIf { it.isNotBlank() },
                entry.author?.takeIf { it.isNotBlank() }?.let { stringResource(R.string.pt_by, it) },
            ).joinToString("\n")
            if (sub.isNotBlank()) Text(sub)
        },
        trailingContent = {
            Switch(checked = entry.isEnabled, onCheckedChange = onCheckedChange)
        },
    )
}
