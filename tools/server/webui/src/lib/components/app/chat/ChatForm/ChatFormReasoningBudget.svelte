<script lang="ts">
	import { config } from '$lib/stores/settings.svelte';
	import { Switch } from '$lib/components/ui/switch';

	interface Props {
		class?: string;
	}

	let { class: className = '' }: Props = $props();
	let currentConfig = config();

	function handleBudgetChange(event: Event) {
		const target = event.target as HTMLSelectElement;
		const value = Number(target.value);
		currentConfig.thinking_budget_tokens = value;
	}

	function handleThinkingToggle(checked: boolean) {
		currentConfig.enable_thinking = checked;
	}
</script>

<div class="flex items-center gap-2 {className}">
	<!-- Toggle Enable/Disable Thinking -->
	<div class="flex items-center gap-1.5">
		<Switch
			id="enable-thinking"
			checked={currentConfig.enable_thinking ?? true}
			onCheckedChange={handleThinkingToggle}
			class="h-4 w-7"
		/>
		<label
			for="enable-thinking"
			class="text-xs text-muted-foreground font-medium cursor-pointer select-none"
		>
			Thinking
		</label>
	</div>

	<!-- Budget Selector (disabled when thinking is off) -->
	<select
		class="h-7 w-[110px] text-xs bg-transparent border border-input rounded-md px-2 py-0.5 focus:outline-none focus:ring-1 focus:ring-ring cursor-pointer disabled:opacity-50 disabled:cursor-not-allowed"
		value={String(currentConfig.thinking_budget_tokens ?? 0)}
		onchange={handleBudgetChange}
		disabled={!(currentConfig.enable_thinking ?? true)}
		title={!(currentConfig.enable_thinking ?? true) ? 'Enable thinking to adjust budget' : 'Select reasoning budget'}
	>
		<option value="0">Unlimited</option>
		<option value="256">Low (256)</option>
		<option value="512">Medium (512)</option>
		<option value="1024">High (1024)</option>
		<option value="2048">Very High (2048)</option>
	</select>
</div>
