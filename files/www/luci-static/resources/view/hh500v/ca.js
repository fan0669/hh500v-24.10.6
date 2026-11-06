/*
 * HH500V 载波 (carrier aggregation) overview.
 *
 * Everything shown here comes from the cache written during the boot AT window
 * (hh500v-sig-cache.sh -> /etc/hh500v-cells.txt + /etc/hh500v-modem-cfg).
 * We cannot query the modem live: as soon as the MBIM session is up this GM800
 * stops answering AT, and its firmware exposes no carrier/SCell field at all
 * (AT+ZCELLINFO=? lists one cell per RAT only, no +ZCA family, no +QCAINFO).
 *
 * So the page answers one question: "how many cells did the modem camp on?"
 * A second NR/LTE line means the network actually gave us an SCell.
 */

'use strict';
'require view';
'require fs';
'require ui';

function run(path, args) {
	return fs.exec(path, args || []).then(function (res) {
		return (res && typeof res.stdout === 'string') ? res.stdout : '';
	});
}

function load_data() {
	return run('/usr/bin/hh500v-ca.uc', []).then(function (txt) {
		try {
			return JSON.parse(txt);
		}
		catch (e) {
			return { error: 'JSON 解析失败：' + (txt ? txt.substr(0, 200) : '(空输出)') };
		}
	});
}

function row(label, value, hint) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td', 'style': 'width:34%' }, [
			label,
			hint ? E('div', { 'class': 'cbi-value-description' }, hint) : ''
		]),
		E('td', { 'class': 'td' }, (value === '' || value === null || value === undefined)
			? '-' : String(value))
	]);
}

/* empty / missing values must render as '-', not as 'undefined dBm' */
function unit(v, suffix) {
	if (v === '' || v === null || v === undefined || v === '-')
		return '-';
	return String(v) + suffix;
}

function badge(ok) {
	return E('span', {
		'class': 'label',
		'style': ok
			? 'background:#4caf50;color:#fff;padding:1px 7px;border-radius:3px;font-size:90%'
			: 'background:#ff9800;color:#fff;padding:1px 7px;border-radius:3px;font-size:90%'
	}, ok ? 'OK' : '注意');
}

function verdict_style(cls) {
	switch (cls) {
		case 'success': return 'background:#e8f5e9;border-left:4px solid #4caf50;padding:8px 12px';
		case 'warning': return 'background:#fff8e1;border-left:4px solid #ffb300;padding:8px 12px';
		default:        return 'background:#e3f2fd;border-left:4px solid #2196f3;padding:8px 12px';
	}
}

function mode_label(zsnt) {
	switch (String(zsnt || '').charAt(0)) {
		case '7': return '仅 5G (NR only)';
		case '8': return 'LTE + 5G (ca)';
		case '6': return '仅 4G (LTE only)';
		case '2': return '仅 3G (WCDMA only)';
		case '0': return '自动 (3G/4G/5G)';
		default:  return (zsnt || '未知');
	}
}

return view.extend({
	load: function () {
		return load_data();
	},

	render: function (data) {
		var self = this;
		var d = data || {};

		if (d.error) {
			return E('div', { 'class': 'cbi-map' }, [
				E('h2', {}, 'HH500V 载波'),
				E('div', { 'class': 'alert-message warning' }, d.error)
			]);
		}

		var cells = d.cells || [];
		var readiness = d.readiness || [];

		var cell_rows = cells.map(function (line, idx) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td' }, String(idx + 1)),
				E('td', { 'class': 'td', 'style': 'word-break:break-all;font-family:monospace;font-size:92%' }, line)
			]);
		});

		if (!cell_rows.length) {
			cell_rows.push(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td', 'colspan': 2 },
					E('em', {}, '还没有小区缓存 —— 下次开机、模组驻网后会自动写入。'))
			]));
		}

		var ready_rows = readiness.map(function (r) {
			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td', 'style': 'width:22%' }, badge(r.ok)),
				E('td', { 'class': 'td', 'style': 'width:22%' }, r.label),
				E('td', { 'class': 'td' }, r.detail)
			]);
		});

		function switch_mode(mode, label) {
			return function () {
				return run('/usr/bin/hh500v-ca-mode.sh', [ mode ]).then(function (out) {
					ui.addNotification(null, E('p', {},
						'已把网络模式设为「' + label + '」。' +
						'它会在下次开机、AT 窗口里下发给模组（数据会话一起来 AT 就静默了）。'),
						'info');
					if (out) console.log(out);
				}).catch(function (err) {
					ui.addNotification(null, E('p', {},
						'切换失败：' + (err && err.message ? err.message : String(err))), 'danger');
				});
			};
		}

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', {}, 'HH500V 载波'),
			E('div', { 'class': 'cbi-map-descr' }, [
				'数据来自每次开机 AT 窗口抓到的 ', E('code', {}, 'AT+ZCELLINFO?'),
				' 原文（模组会为每个驻留小区打一行）。NR/LTE 出现两行 = 网络真的给了辅载波。',
				'注意：本模组固件不提供任何 SCC/CA 字段（', E('code', {}, 'AT+CLAC'),
				' 的 163 条命令里没有 +ZCA*/+QCAINFO），所以“看不到聚合”不等于“没聚合”。'
			]),

			E('div', { 'style': verdict_style(d.verdict_class) + ';margin:12px 0' }, [
				E('strong', {}, d.verdict || '-')
			]),

			E('h3', {}, '服务小区'),
			E('table', { 'class': 'table' }, [
				row('小区数', 'LTE ' + (d.cells_lte || 0) + ' 个 / NR ' + (d.cells_nr || 0) + ' 个'),
				row('网络模式', mode_label(d.zsnt), '已下发到模组的值：' + (d.zsnt_pushed || '还没下发/未知')),
				row('接入技术 (act)', d.act == '13' ? '13 = NR5G' : (d.act || '-')),
				row('频段', d.band ? 'n' + d.band : '-'),
				row('NR-ARFCN', d.arfcn),
				row('PCI', d.pci),
				row('RSRP', unit(d.rsrp, ' dBm')),
				row('RSRQ', unit(d.rsrq, ' dB')),
				row('SINR', unit(d.sinr, ' dB')),
				row('TAC', d.tac, '十六进制'),
				row('CSQ / 注册', (d.csq || '-') + ' / ' + (d.reg || '-'))
			]),

			E('h3', {}, 'CA 就绪清单'),
			E('table', { 'class': 'table' }, ready_rows),

			E('h3', {}, '小区原文'),
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th', 'style': 'width:8%' }, '#'),
					E('th', { 'class': 'th' }, 'AT+ZCELLINFO? 应答')
				])
			].concat(cell_rows)),

			E('h3', {}, '操作'),
			E('div', { 'class': 'cbi-page-actions' }, [
				E('button', {
					'class': 'btn cbi-button cbi-button-action',
					'click': ui.createHandlerFn(this, function () {
						return load_data().then(function (p) {
							ui.addNotification(null, E('p', {},
								'已刷新：LTE ' + (p.cells_lte || 0) + ' 个 / NR ' + (p.cells_nr || 0) + ' 个'), 'info');
							window.location.reload();
						});
					})
				}, '刷新'),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-apply',
					'click': ui.createHandlerFn(this, switch_mode('ca', '允许 LTE+NR（ca）'))
				}, '切到「允许 LTE+NR」'),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-reset',
					'click': ui.createHandlerFn(this, switch_mode('nronly', '仅 5G（NR only）'))
				}, '切到「仅 5G」')
			]),

			E('div', { 'class': 'cbi-page-actions', 'style': 'margin-top:6px' }, [
				E('em', {}, '提示：只锁 5G 时能聚合 SA 的 NR 载波，但 LTE 锚点被禁，NSA(EN-DC) 与 LTE CA 都用不上；' +
					'站点覆盖不确定时选「允许 LTE+NR」更保险。')
			])
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
