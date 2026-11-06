'use strict';
'require view';
'require fs';
'require poll';
'require ui';

function run(args) {
	return fs.exec('/usr/bin/hh500v-sms-list.sh', args).then(function (res) {
		return res && res.stdout ? res.stdout : '{"total":0,"messages":[]}';
	});
}

/* Asking the module to empty its own message store is a two-step thing: AT is
 * only alive before the MBIM session opens, so we can only queue the job here
 * and hh500v-at-port.sh runs AT+CMGD=1,4 during the next boot window. */
function simclear(args) {
	return fs.exec('/usr/bin/hh500v-sim-clear.sh', args).then(function (res) {
		return res && res.stdout ? res.stdout : '';
	});
}

function when(epoch) {
	if (!epoch)
		return '-';
	let d = new Date(epoch * 1000);
	function p(n) { return (n < 10 ? '0' : '') + n; }
	return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' +
	       p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
}

return view.extend({
	load: function () {
		return Promise.all([ run([]), simclear(['status']) ]);
	},

	render: function (data) {
		let msgs = data[0], scstat = data[1] || '';
		let queued = (scstat.indexOf('queued') >= 0);
		let parsed;
		try { parsed = JSON.parse(msgs); } catch (e) { parsed = { total: 0, messages: [] }; }
		let messages = parsed.messages || [];

		let rows = messages.map(function (m) {
			/* m.when is the timestamp inside the SMS itself (GSM 03.40 SCTS),
			 * m.epoch is when we wrote the row - messages imported from the SIM
			 * all share one epoch, which is confusing, so show the SMS time and
			 * keep the import time in the tooltip */
			let stamp = (m.when && m.when != '-') ? m.when : when(m.epoch);

			return E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td', 'title': '入库时间 ' + when(m.epoch) }, stamp),
				E('td', { 'class': 'td' }, m.sender || '-'),
				E('td', { 'class': 'td', 'style': 'white-space:pre-wrap;word-break:break-all' }, m.text || ''),
				E('td', { 'class': 'td' }, [
					E('button', {
						'class': 'btn cbi-button cbi-button-remove',
						'click': ui.createHandlerFn(this, function () {
							return run(['delete', m.id]).then(function () {
								window.location.reload();
							});
						})
					}, '删除')
				])
			]);
		});

		if (!rows.length) {
			rows.push(E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td', 'colspan': 4 },
					E('em', {}, '还没有收到短信。新短信到达时会自动出现在这里。'))
			]));
		}

		return E([], [
			E('h2', {}, 'HH500V 短信'),
			E('div', { 'class': 'cbi-map-descr' }, [
				'直接来自模组 GM800 的 MBIM 通知（不占用 AT 口、不断网），共 ',
				E('strong', {}, String(parsed.total || 0)),
				' 条。'
			]),
			E('div', { 'class': 'cbi-page-actions' }, [
				E('button', {
					'class': 'btn cbi-button cbi-button-action',
					'click': ui.createHandlerFn(this, function () {
						return run([]).then(function (txt) {
							let p = JSON.parse(txt);
							ui.addNotification(null,
								E('p', {}, '已刷新，共 ' + (p.total || 0) + ' 条'), 'info');
							window.location.reload();
						});
					})
				}, '刷新'),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-reset',
					'click': ui.createHandlerFn(this, function () {
						return run(['clear']).then(function () {
							window.location.reload();
						});
					})
				}, '清空全部')
			]),
			E('div', { 'class': 'cbi-page-actions' }, [
				E('button', {
					'class': 'btn cbi-button cbi-button-remove',
					'disabled': queued ? 'disabled' : null,
					'click': ui.createHandlerFn(this, function () {
						if (!confirm('清空 SIM 卡上的全部短信？\n\n' +
						             '· 本机记录会保留（只是历史日志）\n' +
						             '· 真正执行要等下次开机（AT 只在开机窗口可用）\n' +
						             '· 排好后用 hh500v-reset.sh 重启即生效'))
							return;

						return simclear(['request']).then(function (out) {
							ui.addNotification(null, E('p', {},
								'已安排：下次开机时执行 AT+CMGD=1,4。重启用 /usr/bin/hh500v-reset.sh'), 'info');
							window.location.reload();
						});
					})
				}, queued ? '已安排清空 SIM（重启后执行）' : '清空 SIM 卡'),
				queued
					? E('button', {
						'class': 'btn cbi-button cbi-button-neutral',
						'click': ui.createHandlerFn(this, function () {
							return simclear(['cancel']).then(function () {
								window.location.reload();
							});
						})
					}, '取消')
					: ''
			]),
			E('div', { 'class': 'cbi-map-descr' },
				'注：SIM 卡里的短信是模组自己存的（+CPMS: "SM"），这里清的是卡上的那份；' +
				'本页表格是本机的收信日志，清卡不会删它。上次执行结果见 /etc/hh500v-sim-clear.log')
		]);
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, '短信时间'),
					E('th', { 'class': 'th' }, '发件人'),
					E('th', { 'class': 'th' }, '内容'),
					E('th', { 'class': 'th' }, '操作')
				])
			].concat(rows))
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
