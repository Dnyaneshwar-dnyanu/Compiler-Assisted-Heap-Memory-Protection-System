import { useState, useEffect, useCallback } from 'react';
import { apiRequest } from '../services/api.js';
import { checkExecutionHealth } from '../services/executionService.js';

export function useBackendHealth() {
  const [isOnline, setIsOnline] = useState(null); // null = unknown / checking
  const [healthData, setHealthData] = useState(null);
  const [dockerStatus, setDockerStatus] = useState(null);
  const [isChecking, setIsChecking] = useState(false);
  const [lastChecked, setLastChecked] = useState(null);

  const checkHealth = useCallback(async () => {
    setIsChecking(true);
    try {
      const [backendRes, dockerRes] = await Promise.allSettled([
        apiRequest('/api/health', { timeoutMs: 3000 }),
        checkExecutionHealth()
      ]);

      if (backendRes.status === 'fulfilled' && backendRes.value?.success) {
        setIsOnline(true);
        setHealthData(backendRes.value.data);
      } else {
        setIsOnline(false);
        setHealthData(null);
      }

      if (dockerRes.status === 'fulfilled') {
        setDockerStatus(dockerRes.value);
      } else {
        setDockerStatus({ available: false });
      }

      setLastChecked(new Date());
    } catch {
      setIsOnline(false);
      setDockerStatus({ available: false });
    } finally {
      setIsChecking(false);
    }
  }, []);

  useEffect(() => {
    checkHealth();
    // Periodic check every 30 seconds
    const interval = setInterval(checkHealth, 30000);
    return () => clearInterval(interval);
  }, [checkHealth]);

  return {
    isOnline,
    healthData,
    dockerStatus,
    isChecking,
    lastChecked,
    checkHealth
  };
}
